// Copyright 2014, Yahoo! Inc.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "yahoo/cnet/cnet_pool.h"

#include "net/base/network_change_notifier.h"
#include "net/ssl/ssl_config.h"
#include "net/ssl/ssl_config_service.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "yahoo/cnet/cnet_fetcher.h"
#include "yahoo/cnet/cnet_network_delegate.h"
#include "yahoo/cnet/cnet_proxy_service.h"

#if defined(OS_IOS)
#include "net/proxy/proxy_config_service_ios.h"
#elif defined(OS_ANDROID)
#include "net/proxy/proxy_config_service_android.h"
#endif

namespace cnet {

class SSLConfigService : public net::SSLConfigService {
 public:
  SSLConfigService(bool enable_ssl_false_start);

  // Overrides from net::SSLConfigService:
  virtual void GetSSLConfig(net::SSLConfig* config) OVERRIDE;

 private:
  virtual ~SSLConfigService();

  net::SSLConfig config_;

  DISALLOW_COPY_AND_ASSIGN(SSLConfigService);
};

SSLConfigService::SSLConfigService(bool enable_ssl_false_start) {
  config_.false_start_enabled = enable_ssl_false_start;
  config_.require_forward_secrecy = true;
}

SSLConfigService::~SSLConfigService() {
}

void SSLConfigService::GetSSLConfig(net::SSLConfig *config) {
  *config = config_;
}



Pool::PoolContextGetter::PoolContextGetter(net::URLRequestContext* context,
    scoped_refptr<base::SingleThreadTaskRunner> network_runner)
    : context_(context), network_runner_(network_runner) {
}
  
Pool::PoolContextGetter::~PoolContextGetter() {
}

net::URLRequestContext* Pool::PoolContextGetter::GetURLRequestContext() {
  return context_;
}

scoped_refptr<base::SingleThreadTaskRunner>
Pool::PoolContextGetter::GetNetworkTaskRunner() const {
  return network_runner_;
}

Pool::Config::Config()
    : enable_spdy(false), enable_quic(false),
      enable_ssl_false_start(false), trust_all_cert_authorities(false),
      disable_system_proxy(false), cache_max_bytes(0),
      log_level(0) {
}

Pool::Config::~Config() {
}

void PoolTraits::Destruct(const Pool* pool) {
  pool->OnDestruct();
}

Pool::Pool(scoped_refptr<base::SingleThreadTaskRunner> ui_runner,
    const Config& config)
    : proxy_config_service_(NULL), ui_runner_(ui_runner),
      outstanding_requests_(0),
      user_agent_(config.user_agent), enable_spdy_(config.enable_spdy),
      enable_quic_(config.enable_quic),
      enable_ssl_false_start_(config.enable_ssl_false_start),
      disable_system_proxy_(config.disable_system_proxy),
      cache_path_(config.cache_path),
      cache_max_bytes_(config.cache_max_bytes),
      log_level_(config.log_level) {
#ifdef NDEBUG
  trust_all_cert_authorities_ = false;
#else
  trust_all_cert_authorities_ = config.trust_all_cert_authorities;
#endif

  file_thread_ = NULL;
  network_thread_ = new base::Thread("cnet");
  work_thread_ = new base::Thread("cnet-work");
  base::Thread::Options options;
  options.message_loop_type = base::MessageLoop::TYPE_IO;
  network_thread_->StartWithOptions(options);
  work_thread_->StartWithOptions(options);

  GetNetworkTaskRunner()->PostTask(FROM_HERE,
      base::Bind(&Pool::InitializeURLRequestContext, this, ui_runner));
}

Pool::~Pool() {
}

void Pool::OnDestruct() const {
  // We are destructing.  Do no retain this!
  if (!GetNetworkTaskRunner()->RunsTasksOnCurrentThread()) {
    GetNetworkTaskRunner()->PostTask(FROM_HERE,
        base::Bind(&Pool::OnDestruct, base::Unretained(this)));
    return;
  }

  if (ui_runner_ == NULL) {
    LOG(WARNING) << "Leaking CnetPool threads";
  } else {
    // Stopping the threads joins with them, so we have to do this from
    // a different thread.
    ui_runner_->PostTask(FROM_HERE, base::Bind(&Pool::DeleteThreads,
        network_thread_, work_thread_, file_thread_));
  }

  delete this;
}

/* static */
void Pool::DeleteThreads(base::Thread *network, base::Thread *work,
    base::Thread *file) {
  if (file != NULL) {
    file->Stop();
  }
  if (work != NULL) {
    work->Stop();
  }
  if (network != NULL) {
    network->Stop();
  }
}

// LICENSE: modeled after
//    URLRequestContextAdapter::InitializeURLRequestContext() from
//    components/cronet/android/url_request_context_adapter.cc
void Pool::InitializeURLRequestContext(
    scoped_refptr<base::SingleThreadTaskRunner> ui_runner) {
  proxy_config_service_ = new cnet::ProxyConfigService();

  net::URLRequestContextBuilder context_builder;
  context_builder.set_network_delegate(new CnetNetworkDelegate());
  context_builder.set_proxy_config_service(proxy_config_service_);
  context_builder.SetSpdyAndQuicEnabled(enable_spdy_, enable_quic_);
  if (!user_agent_.empty()) {
    context_builder.set_user_agent(user_agent_);
  }

  if (cache_path_.empty() || (cache_max_bytes_ == 0)) {
    context_builder.DisableHttpCache();
  } else {
    net::URLRequestContextBuilder::HttpCacheParams cache_params;
    cache_params.type = net::URLRequestContextBuilder::HttpCacheParams::DISK;
    cache_params.max_size = cache_max_bytes_;
    cache_params.path = cache_path_;
    context_builder.EnableHttpCache(cache_params);
  }

  context_.reset(context_builder.Build());
  context_->set_ssl_config_service(
      new SSLConfigService(enable_ssl_false_start_));

  pool_context_getter_ = new PoolContextGetter(context_.get(),
      GetNetworkTaskRunner());

  AllocSystemProxyOnUi();
}

void Pool::AllocSystemProxyOnUi() {
  if ((ui_runner_ == NULL) || disable_system_proxy_) {
    ActivateSystemProxy(NULL);
    return;
  } else if (!ui_runner_->RunsTasksOnCurrentThread()) {
    ui_runner_->PostTask(FROM_HERE,
        base::Bind(&Pool::AllocSystemProxyOnUi, this));
    return;
  }

  net::ProxyConfigService* system_proxy_service = NULL;
#if defined(OS_IOS)
  system_proxy_service = new net::ProxyConfigServiceIOS();
#elif defined(OS_ANDROID)
  system_proxy_service =
      new net::ProxyConfigServiceAndroid(GetNetworkTaskRunner(), ui_runner_);
#endif
  ActivateSystemProxy(system_proxy_service);
}

void Pool::ActivateSystemProxy(net::ProxyConfigService *system_proxy_service) {
  if (!GetNetworkTaskRunner()->RunsTasksOnCurrentThread()) {
    GetNetworkTaskRunner()->PostTask(FROM_HERE,
        base::Bind(&Pool::ActivateSystemProxy, this, system_proxy_service));
    return;
  }

  DCHECK(proxy_config_service_ != NULL);
  if (proxy_config_service_ != NULL) {
    proxy_config_service_->ActivateSystemProxyService(system_proxy_service);
  }
}

void Pool::SetProxyConfig(const std::string& rules) {
  if (!GetNetworkTaskRunner()->RunsTasksOnCurrentThread()) {
    GetNetworkTaskRunner()->PostTask(FROM_HERE,
        base::Bind(&Pool::SetProxyConfig, this, rules));
    return;
  }

  proxy_config_service_->SetProxyConfig(rules);
}

void Pool::SetTrustAllCertAuthorities(bool value) {
  if (!GetNetworkTaskRunner()->RunsTasksOnCurrentThread()) {
    GetNetworkTaskRunner()->PostTask(FROM_HERE,
        base::Bind(&Pool::SetTrustAllCertAuthorities, this, value));
    return;
  }

  trust_all_cert_authorities_ = value;
}

void Pool::SetEnableSslFalseStart(bool value) {
  if (!GetNetworkTaskRunner()->RunsTasksOnCurrentThread()) {
    GetNetworkTaskRunner()->PostTask(FROM_HERE,
        base::Bind(&Pool::SetEnableSslFalseStart, this, value));
    return;
  }

  enable_ssl_false_start_ = value;
  context_->set_ssl_config_service(
      new SSLConfigService(enable_ssl_false_start_));
}

scoped_refptr<base::SingleThreadTaskRunner> Pool::GetNetworkTaskRunner() const {
  return network_thread_->task_runner();
}

scoped_refptr<base::SingleThreadTaskRunner> Pool::GetWorkTaskRunner() const {
  return work_thread_->task_runner();
}

scoped_refptr<base::SingleThreadTaskRunner> Pool::GetFileTaskRunner() {
  if (file_thread_ == NULL) {
    file_thread_ = new base::Thread("cnet-file");
    base::Thread::Options options;
    options.message_loop_type = base::MessageLoop::TYPE_IO;
    file_thread_->StartWithOptions(options);
  }

  return file_thread_->task_runner();
}

void Pool::AddObserver(Observer* observer) {
  if (!GetNetworkTaskRunner()->RunsTasksOnCurrentThread()) {
    GetNetworkTaskRunner()->PostTask(FROM_HERE,
        base::Bind(&Pool::AddObserver, this, observer));
    return;
  }

  if (observer != NULL) {
    observers_.AddObserver(observer);
    if (outstanding_requests_ == 0) {
      observer->OnPoolIdle(this);
    }
  }
}

void Pool::RemoveObserver(Observer *observer) {
  if (!GetNetworkTaskRunner()->RunsTasksOnCurrentThread()) {
    GetNetworkTaskRunner()->PostTask(FROM_HERE,
        base::Bind(&Pool::RemoveObserver, this, observer));
    return;
  }

  if (observer != NULL) {
    observers_.RemoveObserver(observer);
  }
}

void Pool::TagFetcher(scoped_refptr<Fetcher> fetcher, int tag) {
  if (!GetNetworkTaskRunner()->RunsTasksOnCurrentThread()) {
    GetNetworkTaskRunner()->PostTask(FROM_HERE,
        base::Bind(&Pool::TagFetcher, this, fetcher, tag));
    return;
  }

  tag_to_fetcher_list_[tag].insert(fetcher);
  fetcher_to_tag_[fetcher] = tag;
}

void Pool::CancelTag(int tag) {
  if (!GetNetworkTaskRunner()->RunsTasksOnCurrentThread()) {
    GetNetworkTaskRunner()->PostTask(FROM_HERE,
        base::Bind(&Pool::CancelTag, this, tag));
    return;
  }

  FetcherList fetchers = tag_to_fetcher_list_[tag];
  tag_to_fetcher_list_.erase(tag);

  for (FetcherList::const_iterator it = fetchers.begin(); it != fetchers.end();
       ++it) {
    scoped_refptr<Fetcher> fetcher = *it;
    fetcher->Cancel();
    fetcher_to_tag_.erase(fetcher);
  }
}

void Pool::FetcherStarting(scoped_refptr<cnet::Fetcher> fetcher) {
  if (!GetNetworkTaskRunner()->RunsTasksOnCurrentThread()) {
    GetNetworkTaskRunner()->PostTask(FROM_HERE,
        base::Bind(&Pool::FetcherStarting, this, fetcher));
    return;
  }

  outstanding_requests_++;
}

void Pool::FetcherCompleted(scoped_refptr<Fetcher> fetcher) {
  if (!GetNetworkTaskRunner()->RunsTasksOnCurrentThread()) {
    GetNetworkTaskRunner()->PostTask(FROM_HERE,
        base::Bind(&Pool::FetcherCompleted, this, fetcher));
    return;
  }

  FetcherToTag::iterator it = fetcher_to_tag_.find(fetcher);
  if (it != fetcher_to_tag_.end()) {
    int tag = it->second;
    TagToFetcherList::iterator list_it = tag_to_fetcher_list_.find(tag);
    if (list_it != tag_to_fetcher_list_.end()) {
      FetcherList fetcher_list(list_it->second);
      fetcher_list.erase(fetcher);
      if (fetcher_list.size() == 0) {
        tag_to_fetcher_list_.erase(list_it);
      }
    }
    fetcher_to_tag_.erase(it);
  }

  DCHECK(outstanding_requests_ > 0);
  if (outstanding_requests_ > 0) {
    outstanding_requests_--;
    if (outstanding_requests_ == 0) {
      FOR_EACH_OBSERVER(Observer, observers_, OnPoolIdle(this));
    }
  }
}

} // namespace cnet