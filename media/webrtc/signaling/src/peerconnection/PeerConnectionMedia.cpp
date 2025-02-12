/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <ostream>
#include <string>
#include <vector>

#include "CSFLog.h"

#include "nspr.h"

#include "nricectx.h"
#include "nricemediastream.h"
#include "MediaPipelineFilter.h"
#include "MediaPipeline.h"
#include "PeerConnectionImpl.h"
#include "PeerConnectionMedia.h"
#include "runnable_utils.h"
#include "transportlayerice.h"
#include "transportlayerdtls.h"
#include "transportlayersrtp.h"
#include "signaling/src/jsep/JsepSession.h"
#include "signaling/src/jsep/JsepTransport.h"
#include "signaling/src/mediapipeline/TransportLayerPacketDumper.h"
#include "signaling/src/peerconnection/PacketDumper.h"

#include "nsContentUtils.h"
#include "nsNetCID.h"
#include "nsNetUtil.h"
#include "nsIURI.h"
#include "nsIScriptSecurityManager.h"
#include "nsICancelable.h"
#include "nsILoadInfo.h"
#include "nsIContentPolicy.h"
#include "nsIProxyInfo.h"
#include "nsIProtocolProxyService.h"

#include "nsProxyRelease.h"

#include "nsIScriptGlobalObject.h"
#include "mozilla/Preferences.h"
#include "mozilla/Telemetry.h"
#include "MediaManager.h"
#include "WebrtcGmpVideoCodec.h"

namespace mozilla {
using namespace dom;

static const char* pcmLogTag = "PeerConnectionMedia";
#ifdef LOGTAG
#undef LOGTAG
#endif
#define LOGTAG pcmLogTag

NS_IMETHODIMP PeerConnectionMedia::ProtocolProxyQueryHandler::
OnProxyAvailable(nsICancelable *request,
                 nsIChannel *aChannel,
                 nsIProxyInfo *proxyinfo,
                 nsresult result) {

  if (!pcm_->mProxyRequest) {
    // PeerConnectionMedia is no longer waiting
    return NS_OK;
  }

  CSFLogInfo(LOGTAG, "%s: Proxy Available: %d", __FUNCTION__, (int)result);

  if (NS_SUCCEEDED(result) && proxyinfo) {
    SetProxyOnPcm(*proxyinfo);
  }

  pcm_->mProxyResolveCompleted = true;
  pcm_->mProxyRequest = nullptr;
  pcm_->FlushIceCtxOperationQueueIfReady();

  return NS_OK;
}

void
PeerConnectionMedia::ProtocolProxyQueryHandler::SetProxyOnPcm(
    nsIProxyInfo& proxyinfo)
{
  CSFLogInfo(LOGTAG, "%s: Had proxyinfo", __FUNCTION__);
  nsresult rv;
  nsCString httpsProxyHost;
  int32_t httpsProxyPort;

  rv = proxyinfo.GetHost(httpsProxyHost);
  if (NS_FAILED(rv)) {
    CSFLogError(LOGTAG, "%s: Failed to get proxy server host", __FUNCTION__);
    return;
  }

  rv = proxyinfo.GetPort(&httpsProxyPort);
  if (NS_FAILED(rv)) {
    CSFLogError(LOGTAG, "%s: Failed to get proxy server port", __FUNCTION__);
    return;
  }

  if (pcm_->mIceCtx.get()) {
    assert(httpsProxyPort >= 0 && httpsProxyPort < (1 << 16));
    // Note that this could check if PrivacyRequested() is set on the PC and
    // remove "webrtc" from the ALPN list.  But that would only work if the PC
    // was constructed with a peerIdentity constraint, not when isolated
    // streams are added.  If we ever need to signal to the proxy that the
    // media is isolated, then we would need to restructure this code.
    pcm_->mProxyServer.reset(
      new NrIceProxyServer(httpsProxyHost.get(),
                           static_cast<uint16_t>(httpsProxyPort),
                           "webrtc,c-webrtc"));
  } else {
    CSFLogError(LOGTAG, "%s: Failed to set proxy server (ICE ctx unavailable)",
        __FUNCTION__);
  }
}

NS_IMPL_ISUPPORTS(PeerConnectionMedia::ProtocolProxyQueryHandler, nsIProtocolProxyCallback)

void
PeerConnectionMedia::StunAddrsHandler::OnStunAddrsAvailable(
    const mozilla::net::NrIceStunAddrArray& addrs)
{
  CSFLogInfo(LOGTAG, "%s: receiving (%d) stun addrs", __FUNCTION__,
                                                      (int)addrs.Length());
  if (pcm_) {
    pcm_->mStunAddrs = addrs;
    pcm_->mLocalAddrsCompleted = true;
    pcm_->mStunAddrsRequest = nullptr;
    pcm_->FlushIceCtxOperationQueueIfReady();
    // If parent process returns 0 STUN addresses, change ICE connection
    // state to failed.
    if (!pcm_->mStunAddrs.Length()) {
      pcm_->SignalIceConnectionStateChange(dom::PCImplIceConnectionState::Failed);
    }

    pcm_ = nullptr;
  }
}

PeerConnectionMedia::PeerConnectionMedia(PeerConnectionImpl *parent)
    : mParent(parent),
      mParentHandle(parent->GetHandle()),
      mParentName(parent->GetName()),
      mIceCtx(nullptr),
      mDNSResolver(new NrIceResolver()),
      mUuidGen(MakeUnique<PCUuidGenerator>()),
      mMainThread(mParent->GetMainThread()),
      mSTSThread(mParent->GetSTSThread()),
      mProxyResolveCompleted(false),
      mLocalAddrsCompleted(false) {
}

PeerConnectionMedia::~PeerConnectionMedia()
{
  MOZ_RELEASE_ASSERT(!mMainThread);
}

void
PeerConnectionMedia::InitLocalAddrs()
{
  if (XRE_IsContentProcess()) {
    CSFLogDebug(LOGTAG, "%s: Get stun addresses via IPC",
                mParentHandle.c_str());

    nsCOMPtr<nsIEventTarget> target = mParent->GetWindow()
      ? mParent->GetWindow()->EventTargetFor(TaskCategory::Other)
      : nullptr;

    // We're in the content process, so send a request over IPC for the
    // stun address discovery.
    mStunAddrsRequest =
      new net::StunAddrsRequestChild(new StunAddrsHandler(this), target);
    mStunAddrsRequest->SendGetStunAddrs();
  } else {
    // No content process, so don't need to hold up the ice event queue
    // until completion of stun address discovery. We can let the
    // discovery of stun addresses happen in the same process.
    mLocalAddrsCompleted = true;
  }
}

nsresult
PeerConnectionMedia::InitProxy()
{
  // Allow mochitests to disable this, since mochitest configures a fake proxy
  // that serves up content.
  bool disable = Preferences::GetBool("media.peerconnection.disable_http_proxy",
                                      false);
  if (disable) {
    mProxyResolveCompleted = true;
    return NS_OK;
  }

  nsresult rv;
  nsCOMPtr<nsIProtocolProxyService> pps =
    do_GetService(NS_PROTOCOLPROXYSERVICE_CONTRACTID, &rv);
  if (NS_FAILED(rv)) {
    CSFLogError(LOGTAG, "%s: Failed to get proxy service: %d", __FUNCTION__, (int)rv);
    return NS_ERROR_FAILURE;
  }

  // We use the following URL to find the "default" proxy address for all HTTPS
  // connections.  We will only attempt one HTTP(S) CONNECT per peer connection.
  // "example.com" is guaranteed to be unallocated and should return the best default.
  nsCOMPtr<nsIURI> fakeHttpsLocation;
  rv = NS_NewURI(getter_AddRefs(fakeHttpsLocation), "https://example.com");
  if (NS_FAILED(rv)) {
    CSFLogError(LOGTAG, "%s: Failed to set URI: %d", __FUNCTION__, (int)rv);
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIChannel> channel;
  rv = NS_NewChannel(getter_AddRefs(channel),
                     fakeHttpsLocation,
                     nsContentUtils::GetSystemPrincipal(),
                     nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_DATA_IS_NULL,
                     nsIContentPolicy::TYPE_OTHER);

  if (NS_FAILED(rv)) {
    CSFLogError(LOGTAG, "%s: Failed to get channel from URI: %d",
                __FUNCTION__, (int)rv);
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIEventTarget> target = mParent->GetWindow()
      ? mParent->GetWindow()->EventTargetFor(TaskCategory::Network)
      : nullptr;
  RefPtr<ProtocolProxyQueryHandler> handler = new ProtocolProxyQueryHandler(this);
  rv = pps->AsyncResolve(channel,
                         nsIProtocolProxyService::RESOLVE_PREFER_HTTPS_PROXY |
                         nsIProtocolProxyService::RESOLVE_ALWAYS_TUNNEL,
                         handler, target, getter_AddRefs(mProxyRequest));
  if (NS_FAILED(rv)) {
    CSFLogError(LOGTAG, "%s: Failed to resolve protocol proxy: %d", __FUNCTION__, (int)rv);
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

static nsresult addNrIceServer(const nsString& aIceUrl,
                               const dom::RTCIceServer& aIceServer,
                               std::vector<NrIceStunServer>* aStunServersOut,
                               std::vector<NrIceTurnServer>* aTurnServersOut)
{
  // Without STUN/TURN handlers, NS_NewURI returns nsSimpleURI rather than
  // nsStandardURL. To parse STUN/TURN URI's to spec
  // http://tools.ietf.org/html/draft-nandakumar-rtcweb-stun-uri-02#section-3
  // http://tools.ietf.org/html/draft-petithuguenin-behave-turn-uri-03#section-3
  // we parse out the query-string, and use ParseAuthority() on the rest
  RefPtr<nsIURI> url;
  nsresult rv = NS_NewURI(getter_AddRefs(url), aIceUrl);
  NS_ENSURE_SUCCESS(rv, rv);
  bool isStun = false, isStuns = false, isTurn = false, isTurns = false;
  url->SchemeIs("stun", &isStun);
  url->SchemeIs("stuns", &isStuns);
  url->SchemeIs("turn", &isTurn);
  url->SchemeIs("turns", &isTurns);
  if (!(isStun || isStuns || isTurn || isTurns)) {
    return NS_ERROR_FAILURE;
  }
  if (isStuns) {
    return NS_OK; // TODO: Support STUNS (Bug 1056934)
  }

  nsAutoCString spec;
  rv = url->GetSpec(spec);
  NS_ENSURE_SUCCESS(rv, rv);

  // TODO(jib@mozilla.com): Revisit once nsURI supports STUN/TURN (Bug 833509)
  int32_t port;
  nsAutoCString host;
  nsAutoCString transport;
  {
    uint32_t hostPos;
    int32_t hostLen;
    nsAutoCString path;
    rv = url->GetPathQueryRef(path);
    NS_ENSURE_SUCCESS(rv, rv);

    // Tolerate query-string + parse 'transport=[udp|tcp]' by hand.
    int32_t questionmark = path.FindChar('?');
    if (questionmark >= 0) {
      const nsCString match = NS_LITERAL_CSTRING("transport=");

      for (int32_t i = questionmark, endPos; i >= 0; i = endPos) {
        endPos = path.FindCharInSet("&", i + 1);
        const nsDependentCSubstring fieldvaluepair = Substring(path, i + 1,
            endPos);
        if (StringBeginsWith(fieldvaluepair, match)) {
          transport = Substring(fieldvaluepair, match.Length());
          ToLowerCase(transport);
        }
      }
      path.SetLength(questionmark);
    }

    rv = net_GetAuthURLParser()->ParseAuthority(path.get(), path.Length(),
        nullptr,  nullptr,
        nullptr,  nullptr,
        &hostPos,  &hostLen, &port);
    NS_ENSURE_SUCCESS(rv, rv);
    if (!hostLen) {
      return NS_ERROR_FAILURE;
    }
    if (hostPos > 1) {
       /* The username was removed */
      return NS_ERROR_FAILURE;
    }
    path.Mid(host, hostPos, hostLen);
  }
  if (port == -1) {
    port = (isStuns || isTurns)? 5349 : 3478;
  }

  if (isStuns || isTurns) {
    // Should we barf if transport is set to udp or something?
    transport = kNrIceTransportTls;
  }

  if (transport.IsEmpty()) {
    transport = kNrIceTransportUdp;
  }

  if (isTurn || isTurns) {
    std::string pwd(NS_ConvertUTF16toUTF8(aIceServer.mCredential.Value()).get());
    std::string username(NS_ConvertUTF16toUTF8(aIceServer.mUsername.Value()).get());

    std::vector<unsigned char> password(pwd.begin(), pwd.end());

    UniquePtr<NrIceTurnServer> server(NrIceTurnServer::Create(host.get(), port, username, password, transport.get()));
    if (!server) {
      return NS_ERROR_FAILURE;
    }
    aTurnServersOut->emplace_back(std::move(*server));
  } else {
    UniquePtr<NrIceStunServer> server(NrIceStunServer::Create(host.get(), port, transport.get()));
    if (!server) {
      return NS_ERROR_FAILURE;
    }
    aStunServersOut->emplace_back(std::move(*server));
  }
  return NS_OK;
}

static NrIceCtx::Policy toNrIcePolicy(dom::RTCIceTransportPolicy aPolicy)
{
  switch (aPolicy) {
    case dom::RTCIceTransportPolicy::Relay:
      return NrIceCtx::ICE_POLICY_RELAY;
    case dom::RTCIceTransportPolicy::All:
      if (Preferences::GetBool("media.peerconnection.ice.no_host", false)) {
        return NrIceCtx::ICE_POLICY_NO_HOST;
      } else {
        return NrIceCtx::ICE_POLICY_ALL;
      }
    default:
      MOZ_CRASH();
  }
  return NrIceCtx::ICE_POLICY_ALL;
}

nsresult PeerConnectionMedia::Init(const dom::RTCConfiguration& aConfiguration)
{
  nsresult rv = InitProxy();
  NS_ENSURE_SUCCESS(rv, rv);

  bool ice_tcp = Preferences::GetBool("media.peerconnection.ice.tcp", false);

  // setup the stun local addresses IPC async call
  InitLocalAddrs();

  NrIceCtx::InitializeGlobals(mParent->GetAllowIceLoopback(),
      ice_tcp,
      mParent->GetAllowIceLinkLocal());

  // TODO(ekr@rtfm.com): need some way to set not offerer later
  // Looks like a bug in the NrIceCtx API.
  mIceCtx = NrIceCtx::Create("PC:" + mParentName,
      toNrIcePolicy(aConfiguration.mIceTransportPolicy));
  if(!mIceCtx) {
    CSFLogError(LOGTAG, "%s: Failed to create Ice Context", __FUNCTION__);
    return NS_ERROR_FAILURE;
  }

  std::vector<NrIceStunServer> stunServers;
  std::vector<NrIceTurnServer> turnServers;

  if (aConfiguration.mIceServers.WasPassed()) {
    for (const auto& iceServer : aConfiguration.mIceServers.Value()) {
      NS_ENSURE_STATE(iceServer.mUrls.WasPassed());
      NS_ENSURE_STATE(iceServer.mUrls.Value().IsStringSequence());
      for (const auto& iceUrl : iceServer.mUrls.Value().GetAsStringSequence()) {
        rv = addNrIceServer(iceUrl, iceServer, &stunServers, &turnServers);
        if (NS_FAILED(rv)) {
          CSFLogError(LOGTAG, "%s: invalid STUN/TURN server: %s",
                      __FUNCTION__, NS_ConvertUTF16toUTF8(iceUrl).get());
          return rv;
        }
      }
    }
  }

  if (NS_FAILED(rv = mIceCtx->SetStunServers(stunServers))) {
    CSFLogError(LOGTAG, "%s: Failed to set stun servers", __FUNCTION__);
    return rv;
  }
  // Give us a way to globally turn off TURN support
  bool disabled = Preferences::GetBool("media.peerconnection.turn.disable", false);
  if (!disabled) {
    if (NS_FAILED(rv = mIceCtx->SetTurnServers(turnServers))) {
      CSFLogError(LOGTAG, "%s: Failed to set turn servers", __FUNCTION__);
      return rv;
    }
  } else if (!turnServers.empty()) {
    CSFLogError(LOGTAG, "%s: Setting turn servers disabled", __FUNCTION__);
  }
  if (NS_FAILED(rv = mDNSResolver->Init())) {
    CSFLogError(LOGTAG, "%s: Failed to initialize dns resolver", __FUNCTION__);
    return rv;
  }
  if (NS_FAILED(rv =
        mIceCtx->SetResolver(mDNSResolver->AllocateResolver()))) {
    CSFLogError(LOGTAG, "%s: Failed to get dns resolver", __FUNCTION__);
    return rv;
  }
  ConnectSignals(mIceCtx.get());
  return NS_OK;
}

void
PeerConnectionMedia::EnsureTransports(const JsepSession& aSession)
{
  for (const auto& transceiver : aSession.GetTransceivers()) {
    if (transceiver->HasOwnTransport()) {
      RUN_ON_THREAD(
          GetSTSThread(),
          WrapRunnable(RefPtr<PeerConnectionMedia>(this),
                       &PeerConnectionMedia::EnsureTransport_s,
                       transceiver->mTransport.mTransportId,
                       transceiver->mTransport.mLocalUfrag,
                       transceiver->mTransport.mLocalPwd,
                       transceiver->mTransport.mComponents),
          NS_DISPATCH_NORMAL);
    }
  }

  GatherIfReady();
}

void
PeerConnectionMedia::EnsureTransport_s(const std::string& aTransportId,
                                       const std::string& aUfrag,
                                       const std::string& aPwd,
                                       size_t aComponentCount)
{
  RefPtr<NrIceMediaStream> stream(mIceCtx->GetStream(aTransportId));
  if (!stream) {
    CSFLogDebug(LOGTAG, "%s: Creating ICE media stream=%s components=%u",
                mParentHandle.c_str(),
                aTransportId.c_str(),
                static_cast<unsigned>(aComponentCount));

    std::ostringstream os;
    os << mParentName << " transport-id=" << aTransportId;
    stream = mIceCtx->CreateStream(aTransportId,
                                   os.str(),
                                   aComponentCount);

    if (!stream) {
      CSFLogError(LOGTAG, "Failed to create ICE stream.");
      return;
    }

    stream->SignalReady.connect(this, &PeerConnectionMedia::IceStreamReady_s);
    stream->SignalCandidate.connect(this,
                                    &PeerConnectionMedia::OnCandidateFound_s);
  }
  // This might begin an ICE restart
  stream->SetIceCredentials(aUfrag, aPwd);
}

nsresult
PeerConnectionMedia::UpdateTransports(const JsepSession& aSession,
                                      const bool forceIceTcp)
{
  std::set<std::string> finalTransports;
  for (const auto& transceiver : aSession.GetTransceivers()) {
    if (transceiver->HasOwnTransport()) {
      finalTransports.insert(transceiver->mTransport.mTransportId);
      UpdateTransport(*transceiver, forceIceTcp);
    }
  }

  RUN_ON_THREAD(
      GetSTSThread(),
      WrapRunnable(RefPtr<PeerConnectionMedia>(this),
                   &PeerConnectionMedia::RemoveTransportsExcept_s,
                   finalTransports),
      NS_DISPATCH_NORMAL);

  for (const auto& transceiverImpl : mTransceivers) {
    transceiverImpl->UpdateTransport(*this);
  }

  return NS_OK;
}

nsresult
PeerConnectionMedia::UpdateTransport(const JsepTransceiver& aTransceiver,
                                     bool aForceIceTcp)
{
  nsresult rv = UpdateTransportFlows(aTransceiver);
  if (NS_FAILED(rv)) {
    return rv;
  }

  std::string ufrag;
  std::string pwd;
  std::vector<std::string> candidates;
  size_t components = 0;

  const JsepTransport& transport = aTransceiver.mTransport;
  unsigned level = aTransceiver.GetLevel();

  CSFLogDebug(LOGTAG, "ACTIVATING TRANSPORT! - PC %s: level=%u components=%u",
              mParentHandle.c_str(), (unsigned)level,
              (unsigned)transport.mComponents);

  ufrag = transport.mIce->GetUfrag();
  pwd = transport.mIce->GetPassword();
  candidates = transport.mIce->GetCandidates();
  components = transport.mComponents;
  if (aForceIceTcp) {
    candidates.erase(std::remove_if(candidates.begin(),
                                    candidates.end(),
                                    [](const std::string & s) {
                                      return s.find(" UDP ") != std::string::npos ||
                                             s.find(" udp ") != std::string::npos; }),
                     candidates.end());
  }

  RUN_ON_THREAD(
      GetSTSThread(),
      WrapRunnable(RefPtr<PeerConnectionMedia>(this),
                   &PeerConnectionMedia::ActivateTransport_s,
                   transport.mTransportId,
                   transport.mLocalUfrag,
                   transport.mLocalPwd,
                   components,
                   ufrag,
                   pwd,
                   candidates),
      NS_DISPATCH_NORMAL);

  return NS_OK;
}

void
PeerConnectionMedia::ActivateTransport_s(
    const std::string& aTransportId,
    const std::string& aLocalUfrag,
    const std::string& aLocalPwd,
    size_t aComponentCount,
    const std::string& aUfrag,
    const std::string& aPassword,
    const std::vector<std::string>& aCandidateList) {

  MOZ_ASSERT(aComponentCount);

  RefPtr<NrIceMediaStream> stream(mIceCtx->GetStream(aTransportId));
  if (!stream) {
    MOZ_ASSERT(false);
    return;
  }

  CSFLogDebug(LOGTAG, "%s: Activating ICE media stream=%s components=%u",
              mParentHandle.c_str(),
              aTransportId.c_str(),
              static_cast<unsigned>(aComponentCount));

  std::vector<std::string> attrs;
  attrs.reserve(aCandidateList.size() + 2 /* ufrag + pwd */);
  for (const auto& candidate : aCandidateList) {
    attrs.push_back("candidate:" + candidate);
  }
  attrs.push_back("ice-ufrag:" + aUfrag);
  attrs.push_back("ice-pwd:" + aPassword);

  nsresult rv = stream->ConnectToPeer(aLocalUfrag, aLocalPwd, attrs);
  if (NS_FAILED(rv)) {
    CSFLogError(LOGTAG, "Couldn't parse ICE attributes, rv=%u",
                        static_cast<unsigned>(rv));
  }

  for (size_t c = aComponentCount; c < stream->components(); ++c) {
    // components are 1-indexed
    stream->DisableComponent(c + 1);
  }
}

void
PeerConnectionMedia::RemoveTransportsExcept_s(
    const std::set<std::string>& aIds)
{
  for (const auto& stream : mIceCtx->GetStreams()) {
    if (!aIds.count(stream->GetId())) {
      mIceCtx->DestroyStream(stream->GetId());
    }
  }
}

nsresult
PeerConnectionMedia::UpdateMediaPipelines()
{
  // The GMP code is all the way on the other side of webrtc.org, and it is not
  // feasible to plumb error information all the way back. So, we set up a
  // handle to the PC (for the duration of this call) in a global variable.
  // This allows the GMP code to report errors to the PC.
  WebrtcGmpPCHandleSetter setter(mParentHandle);

  for (RefPtr<TransceiverImpl>& transceiver : mTransceivers) {
    nsresult rv = transceiver->UpdateConduit();
    if (NS_FAILED(rv)) {
      return rv;
    }

    if (!transceiver->IsVideo()) {
      rv = transceiver->SyncWithMatchingVideoConduits(mTransceivers);
      if (NS_FAILED(rv)) {
        return rv;
      }
      // TODO: If there is no audio, we should probably de-sync. However, this
      // has never been done before, and it is unclear whether it is safe...
    }
  }

  return NS_OK;
}

nsresult
PeerConnectionMedia::UpdateTransportFlows(const JsepTransceiver& aTransceiver)
{
  nsresult rv = UpdateTransportFlow(false, aTransceiver.mTransport);
  if (NS_FAILED(rv)) {
    return rv;
  }

  return UpdateTransportFlow(true, aTransceiver.mTransport);
}

// Accessing the PCMedia should be safe here because we shouldn't
// have enqueued this function unless it was still active and
// the ICE data is destroyed on the STS.
static void
FinalizeTransportFlow_s(const RefPtr<NrIceCtx>& aIceCtx,
                        nsAutoPtr<PacketDumper> aPacketDumper,
                        const RefPtr<TransportFlow>& aFlow,
                        const std::string& aId,
                        bool aIsRtcp,
                        TransportLayerIce* aIceLayer,
                        TransportLayerDtls* aDtlsLayer,
                        TransportLayerSrtp* aSrtpLayer)
{
  TransportLayerPacketDumper* srtpDumper(new TransportLayerPacketDumper(
        std::move(aPacketDumper), dom::mozPacketDumpType::Srtp));

  aIceLayer->SetParameters(aIceCtx->GetStream(aId), aIsRtcp ? 2 : 1);
  // TODO(bug 854518): Process errors.
  (void)aIceLayer->Init();
  (void)aDtlsLayer->Init();
  (void)srtpDumper->Init();
  (void)aSrtpLayer->Init();
  aDtlsLayer->Chain(aIceLayer);
  srtpDumper->Chain(aIceLayer);
  aSrtpLayer->Chain(srtpDumper);
  aFlow->PushLayer(aIceLayer);
  aFlow->PushLayer(aDtlsLayer);
  aFlow->PushLayer(srtpDumper);
  aFlow->PushLayer(aSrtpLayer);
}

nsresult
PeerConnectionMedia::UpdateTransportFlow(bool aIsRtcp,
                                         const JsepTransport& aTransport)
{
  if (aIsRtcp && aTransport.mComponents < 2) {
    RemoveTransportFlow(aTransport.mTransportId, aIsRtcp);
    return NS_OK;
  }

  if (!aIsRtcp && !aTransport.mComponents) {
    RemoveTransportFlow(aTransport.mTransportId, aIsRtcp);
    return NS_OK;
  }

  MOZ_ASSERT(!aTransport.mTransportId.empty());

  nsresult rv;

  RefPtr<TransportFlow> flow = GetTransportFlow(aTransport.mTransportId, aIsRtcp);
  if (flow) {
    return NS_OK;
  }

  std::ostringstream osId;
  osId << mParentHandle << ":" << aTransport.mTransportId << ","
    << (aIsRtcp ? "rtcp" : "rtp");
  flow = new TransportFlow(osId.str());

  // The media streams are made on STS so we need to defer setup.
  auto ice = MakeUnique<TransportLayerIce>();
  auto dtls = MakeUnique<TransportLayerDtls>();
  auto srtp = MakeUnique<TransportLayerSrtp>(*dtls);
  dtls->SetRole(aTransport.mDtls->GetRole() ==
                        JsepDtlsTransport::kJsepDtlsClient
                    ? TransportLayerDtls::CLIENT
                    : TransportLayerDtls::SERVER);

  RefPtr<DtlsIdentity> pcid = mParent->Identity();
  if (!pcid) {
    CSFLogError(LOGTAG, "Failed to get DTLS identity.");
    return NS_ERROR_FAILURE;
  }
  dtls->SetIdentity(pcid);

  const SdpFingerprintAttributeList& fingerprints =
      aTransport.mDtls->GetFingerprints();
  for (const auto& fingerprint : fingerprints.mFingerprints) {
    std::ostringstream ss;
    ss << fingerprint.hashFunc;
    rv = dtls->SetVerificationDigest(ss.str(), &fingerprint.fingerprint[0],
                                     fingerprint.fingerprint.size());
    if (NS_FAILED(rv)) {
      CSFLogError(LOGTAG, "Could not set fingerprint");
      return rv;
    }
  }

  std::vector<uint16_t> srtpCiphers;
  srtpCiphers.push_back(kDtlsSrtpAeadAes256Gcm);
  srtpCiphers.push_back(kDtlsSrtpAeadAes128Gcm);
  srtpCiphers.push_back(kDtlsSrtpAes128CmHmacSha1_80);
  srtpCiphers.push_back(kDtlsSrtpAes128CmHmacSha1_32);

  rv = dtls->SetSrtpCiphers(srtpCiphers);
  if (NS_FAILED(rv)) {
    CSFLogError(LOGTAG, "Couldn't set SRTP ciphers");
    return rv;
  }

  // Always permits negotiation of the confidential mode.
  // Only allow non-confidential (which is an allowed default),
  // if we aren't confidential.
  std::set<std::string> alpn;
  std::string alpnDefault = "";
  alpn.insert("c-webrtc");
  if (!mParent->PrivacyRequested()) {
    alpnDefault = "webrtc";
    alpn.insert(alpnDefault);
  }
  rv = dtls->SetAlpn(alpn, alpnDefault);
  if (NS_FAILED(rv)) {
    CSFLogError(LOGTAG, "Couldn't set ALPN");
    return rv;
  }

  nsAutoPtr<PacketDumper> packetDumper(new PacketDumper(mParent));

  rv = GetSTSThread()->Dispatch(
      WrapRunnableNM(FinalizeTransportFlow_s, mIceCtx, packetDumper, flow,
                     aTransport.mTransportId, aIsRtcp,
                     ice.release(), dtls.release(), srtp.release()),
      NS_DISPATCH_NORMAL);
  if (NS_FAILED(rv)) {
    CSFLogError(LOGTAG, "Failed to dispatch FinalizeTransportFlow_s");
    return rv;
  }

  AddTransportFlow(aTransport.mTransportId, aIsRtcp, flow);

  return NS_OK;
}

void
PeerConnectionMedia::StartIceChecks(const JsepSession& aSession)
{
  nsCOMPtr<nsIRunnable> runnable(
      WrapRunnable(
        RefPtr<PeerConnectionMedia>(this),
        &PeerConnectionMedia::StartIceChecks_s,
        aSession.IsIceControlling(),
        aSession.IsOfferer(),
        aSession.RemoteIsIceLite(),
        // Copy, just in case API changes to return a ref
        std::vector<std::string>(aSession.GetIceOptions())));

  PerformOrEnqueueIceCtxOperation(runnable);
}

void
PeerConnectionMedia::StartIceChecks_s(
    bool aIsControlling,
    bool aIsOfferer,
    bool aIsIceLite,
    const std::vector<std::string>& aIceOptionsList) {

  CSFLogDebug(LOGTAG, "Starting ICE Checking");

  std::vector<std::string> attributes;
  if (aIsIceLite) {
    attributes.push_back("ice-lite");
  }

  if (!aIceOptionsList.empty()) {
    attributes.push_back("ice-options:");
    for (const auto& option : aIceOptionsList) {
      attributes.back() += option + ' ';
    }
  }

  nsresult rv = mIceCtx->ParseGlobalAttributes(attributes);
  if (NS_FAILED(rv)) {
    CSFLogError(LOGTAG, "%s: couldn't parse global parameters", __FUNCTION__ );
  }

  mIceCtx->SetControlling(aIsControlling ?
                                     NrIceCtx::ICE_CONTROLLING :
                                     NrIceCtx::ICE_CONTROLLED);

  mIceCtx->StartChecks(aIsOfferer);
}

bool
PeerConnectionMedia::GetPrefDefaultAddressOnly() const
{
  ASSERT_ON_THREAD(mMainThread); // will crash on STS thread

  uint64_t winId = mParent->GetWindow()->WindowID();

  bool default_address_only = Preferences::GetBool(
    "media.peerconnection.ice.default_address_only", false);
  default_address_only |=
    !MediaManager::Get()->IsActivelyCapturingOrHasAPermission(winId);
  return default_address_only;
}

bool
PeerConnectionMedia::GetPrefProxyOnly() const
{
  ASSERT_ON_THREAD(mMainThread); // will crash on STS thread

  return Preferences::GetBool("media.peerconnection.ice.proxy_only", false);
}

void
PeerConnectionMedia::ConnectSignals(NrIceCtx *aCtx, NrIceCtx *aOldCtx)
{
  aCtx->SignalGatheringStateChange.connect(
      this,
      &PeerConnectionMedia::IceGatheringStateChange_s);
  aCtx->SignalConnectionStateChange.connect(
      this,
      &PeerConnectionMedia::IceConnectionStateChange_s);

  if (aOldCtx) {
    MOZ_ASSERT(aCtx != aOldCtx);
    aOldCtx->SignalGatheringStateChange.disconnect(this);
    aOldCtx->SignalConnectionStateChange.disconnect(this);

    // if the old and new connection state and/or gathering state is
    // different fire the state update.  Note: we don't fire the update
    // if the state is *INIT since updates for the INIT state aren't
    // sent during the normal flow. (mjf)
    if (aOldCtx->connection_state() != aCtx->connection_state() &&
        aCtx->connection_state() != NrIceCtx::ICE_CTX_INIT) {
      aCtx->SignalConnectionStateChange(aCtx, aCtx->connection_state());
    }

    if (aOldCtx->gathering_state() != aCtx->gathering_state() &&
        aCtx->gathering_state() != NrIceCtx::ICE_CTX_GATHER_INIT) {
      aCtx->SignalGatheringStateChange(aCtx, aCtx->gathering_state());
    }
  }
}

void
PeerConnectionMedia::AddIceCandidate(const std::string& candidate,
                                     const std::string& aTransportId) {
  MOZ_ASSERT(!aTransportId.empty());
  RUN_ON_THREAD(GetSTSThread(),
                WrapRunnable(
                    RefPtr<PeerConnectionMedia>(this),
                    &PeerConnectionMedia::AddIceCandidate_s,
                    std::string(candidate), // Make copies.
                    std::string(aTransportId)),
                NS_DISPATCH_NORMAL);
}

void
PeerConnectionMedia::AddIceCandidate_s(const std::string& aCandidate,
                                       const std::string& aTransportId) {
  RefPtr<NrIceMediaStream> stream(mIceCtx->GetStream(aTransportId));
  if (!stream) {
    CSFLogError(LOGTAG, "No ICE stream for candidate with transport id %s: %s",
                        aTransportId.c_str(), aCandidate.c_str());
    return;
  }

  nsresult rv = stream->ParseTrickleCandidate(aCandidate);
  if (NS_FAILED(rv)) {
    CSFLogError(LOGTAG, "Couldn't process ICE candidate with transport id %s: "
                        "%s",
                        aTransportId.c_str(), aCandidate.c_str());
    return;
  }
}

void
PeerConnectionMedia::UpdateNetworkState(bool online) {
  RUN_ON_THREAD(GetSTSThread(),
                WrapRunnable(
                    RefPtr<PeerConnectionMedia>(this),
                    &PeerConnectionMedia::UpdateNetworkState_s,
                    online),
                NS_DISPATCH_NORMAL);
}

void
PeerConnectionMedia::UpdateNetworkState_s(bool online) {
  mIceCtx->UpdateNetworkState(online);
}

void
PeerConnectionMedia::FlushIceCtxOperationQueueIfReady()
{
  ASSERT_ON_THREAD(mMainThread);

  if (IsIceCtxReady()) {
    for (auto& mQueuedIceCtxOperation : mQueuedIceCtxOperations) {
      GetSTSThread()->Dispatch(mQueuedIceCtxOperation, NS_DISPATCH_NORMAL);
    }
    mQueuedIceCtxOperations.clear();
  }
}

void
PeerConnectionMedia::PerformOrEnqueueIceCtxOperation(nsIRunnable* runnable)
{
  ASSERT_ON_THREAD(mMainThread);

  if (IsIceCtxReady()) {
    GetSTSThread()->Dispatch(runnable, NS_DISPATCH_NORMAL);
  } else {
    mQueuedIceCtxOperations.push_back(runnable);
  }
}

void
PeerConnectionMedia::GatherIfReady() {
  ASSERT_ON_THREAD(mMainThread);

  nsCOMPtr<nsIRunnable> runnable(WrapRunnable(
        RefPtr<PeerConnectionMedia>(this),
        &PeerConnectionMedia::EnsureIceGathering_s,
        GetPrefDefaultAddressOnly(),
        GetPrefProxyOnly()));

  PerformOrEnqueueIceCtxOperation(runnable);
}

void
PeerConnectionMedia::EnsureIceGathering_s(bool aDefaultRouteOnly,
                                          bool aProxyOnly) {
  if (mProxyServer) {
    mIceCtx->SetProxyServer(*mProxyServer);
  } else if (aProxyOnly) {
    IceGatheringStateChange_s(mIceCtx.get(),
                              NrIceCtx::ICE_CTX_GATHER_COMPLETE);
    return;
  }

  // Make sure we don't call NrIceCtx::StartGathering if we're in e10s mode
  // and we received no STUN addresses from the parent process.  In the
  // absence of previously provided STUN addresses, StartGathering will
  // attempt to gather them (as in non-e10s mode), and this will cause a
  // sandboxing exception in e10s mode.
  if (!mStunAddrs.Length() && XRE_IsContentProcess()) {
    CSFLogInfo(LOGTAG,
               "%s: No STUN addresses returned from parent process",
               __FUNCTION__);
    return;
  }

  // Belt and suspenders - in e10s mode, the call below to SetStunAddrs
  // needs to have the proper flags set on ice ctx.  For non-e10s,
  // setting those flags happens in StartGathering.  We could probably
  // just set them here, and only do it here.
  mIceCtx->SetCtxFlags(aDefaultRouteOnly, aProxyOnly);

  if (mStunAddrs.Length()) {
    mIceCtx->SetStunAddrs(mStunAddrs);
  }

  // Start gathering, but only if there are streams
  if (!mIceCtx->GetStreams().empty()) {
    mIceCtx->StartGathering(aDefaultRouteOnly, aProxyOnly);
    return;
  }

  CSFLogWarn(LOGTAG,
             "%s: No streams to start gathering on. Can happen with rollback",
             __FUNCTION__);
  // If there are no streams, we're probably in a situation where we've rolled
  // back while still waiting for our proxy configuration to come back. Make
  // sure content knows that the rollback has stuck wrt gathering.
  IceGatheringStateChange_s(mIceCtx.get(),
                            NrIceCtx::ICE_CTX_GATHER_COMPLETE);
}

void
PeerConnectionMedia::SelfDestruct()
{
  ASSERT_ON_THREAD(mMainThread);

  CSFLogDebug(LOGTAG, "%s: ", __FUNCTION__);

  if (mStunAddrsRequest) {
    mStunAddrsRequest->Cancel();
    mStunAddrsRequest = nullptr;
  }

  if (mProxyRequest) {
    mProxyRequest->Cancel(NS_ERROR_ABORT);
    mProxyRequest = nullptr;
  }

  for (auto& transceiver : mTransceivers) {
    // transceivers are garbage-collected, so we need to poke them to perform
    // cleanup right now so the appropriate events fire.
    transceiver->Shutdown_m();
  }

  mTransceivers.clear();

  mQueuedIceCtxOperations.clear();

  // Shutdown the transport (async)
  RUN_ON_THREAD(mSTSThread, WrapRunnable(
      this, &PeerConnectionMedia::ShutdownMediaTransport_s),
                NS_DISPATCH_NORMAL);

  CSFLogDebug(LOGTAG, "%s: Media shut down", __FUNCTION__);
}

void
PeerConnectionMedia::SelfDestruct_m()
{
  CSFLogDebug(LOGTAG, "%s: ", __FUNCTION__);

  ASSERT_ON_THREAD(mMainThread);

  mMainThread = nullptr;

  // Final self-destruct.
  this->Release();
}

void
PeerConnectionMedia::ShutdownMediaTransport_s()
{
  ASSERT_ON_THREAD(mSTSThread);

  CSFLogDebug(LOGTAG, "%s: ", __FUNCTION__);

  disconnect_all();
  mTransportFlows.clear();

#if !defined(MOZILLA_EXTERNAL_LINKAGE)
  NrIceStats stats = mIceCtx->Destroy();

  CSFLogDebug(LOGTAG, "Ice Telemetry: stun (retransmits: %d)"
                      "   turn (401s: %d   403s: %d   438s: %d)",
              stats.stun_retransmits, stats.turn_401s, stats.turn_403s,
              stats.turn_438s);

  Telemetry::ScalarAdd(Telemetry::ScalarID::WEBRTC_NICER_STUN_RETRANSMITS,
                       stats.stun_retransmits);
  Telemetry::ScalarAdd(Telemetry::ScalarID::WEBRTC_NICER_TURN_401S,
                       stats.turn_401s);
  Telemetry::ScalarAdd(Telemetry::ScalarID::WEBRTC_NICER_TURN_403S,
                       stats.turn_403s);
  Telemetry::ScalarAdd(Telemetry::ScalarID::WEBRTC_NICER_TURN_438S,
                       stats.turn_438s);
#endif

  mIceCtx = nullptr;

  // we're holding a ref to 'this' that's released by SelfDestruct_m
  mMainThread->Dispatch(WrapRunnable(this, &PeerConnectionMedia::SelfDestruct_m),
                        NS_DISPATCH_NORMAL);
}


void
PeerConnectionMedia::GetIceStats_s(
    const std::string& aTransportId,
    bool internalStats,
    DOMHighResTimeStamp now,
    RTCStatsReportInternal* report) const
{
  if (mIceCtx) {
    auto stream = mIceCtx->GetStream(aTransportId);
    if (stream) {
      GetIceStats_s(*stream, internalStats, now, report);
    }
  }
}

void
PeerConnectionMedia::GetAllIceStats_s(
    bool internalStats,
    DOMHighResTimeStamp now,
    RTCStatsReportInternal* report) const
{
  if (mIceCtx) {
    for (const auto& stream : mIceCtx->GetStreams()) {
      GetIceStats_s(*stream, internalStats, now, report);
    }
  }
}

static void ToRTCIceCandidateStats(
    const std::vector<NrIceCandidate>& candidates,
    RTCStatsType candidateType,
    const nsString& componentId,
    DOMHighResTimeStamp now,
    RTCStatsReportInternal* report) {

  MOZ_ASSERT(report);
  for (const auto& candidate : candidates) {
    RTCIceCandidateStats cand;
    cand.mType.Construct(candidateType);
    NS_ConvertASCIItoUTF16 codeword(candidate.codeword.c_str());
    cand.mComponentId.Construct(componentId);
    cand.mId.Construct(codeword);
    cand.mTimestamp.Construct(now);
    cand.mCandidateType.Construct(
        RTCStatsIceCandidateType(candidate.type));
    cand.mIpAddress.Construct(
        NS_ConvertASCIItoUTF16(candidate.cand_addr.host.c_str()));
    cand.mPortNumber.Construct(candidate.cand_addr.port);
    cand.mTransport.Construct(
        NS_ConvertASCIItoUTF16(candidate.cand_addr.transport.c_str()));
    if (candidateType == RTCStatsType::Local_candidate) {
      cand.mMozLocalTransport.Construct(
          NS_ConvertASCIItoUTF16(candidate.local_addr.transport.c_str()));
      if (RTCStatsIceCandidateType(candidate.type) == RTCStatsIceCandidateType::Relayed) {
        cand.mRelayProtocol.Construct(
            NS_ConvertASCIItoUTF16(candidate.local_addr.transport.c_str()));
      }
    }
    report->mIceCandidateStats.Value().AppendElement(cand, fallible);
    if (candidate.trickled) {
      report->mTrickledIceCandidateStats.Value().AppendElement(cand, fallible);
    }
  }
}

void
PeerConnectionMedia::GetIceStats_s(
    const NrIceMediaStream& aStream,
    bool internalStats,
    DOMHighResTimeStamp now,
    RTCStatsReportInternal* report) const
{
  NS_ConvertASCIItoUTF16 transportId(aStream.GetId().c_str());

  std::vector<NrIceCandidatePair> candPairs;
  nsresult res = aStream.GetCandidatePairs(&candPairs);
  if (NS_FAILED(res)) {
    CSFLogError(LOGTAG,
        "%s: Error getting candidate pairs for transport id \"%s\"",
        __FUNCTION__, aStream.GetId().c_str());
    return;
  }

  for (auto& candPair : candPairs) {
    NS_ConvertASCIItoUTF16 codeword(candPair.codeword.c_str());
    NS_ConvertASCIItoUTF16 localCodeword(candPair.local.codeword.c_str());
    NS_ConvertASCIItoUTF16 remoteCodeword(candPair.remote.codeword.c_str());
    // Only expose candidate-pair statistics to chrome, until we've thought
    // through the implications of exposing it to content.

    RTCIceCandidatePairStats s;
    s.mId.Construct(codeword);
    s.mTransportId.Construct(transportId);
    s.mTimestamp.Construct(now);
    s.mType.Construct(RTCStatsType::Candidate_pair);
    s.mLocalCandidateId.Construct(localCodeword);
    s.mRemoteCandidateId.Construct(remoteCodeword);
    s.mNominated.Construct(candPair.nominated);
    s.mWritable.Construct(candPair.writable);
    s.mReadable.Construct(candPair.readable);
    s.mPriority.Construct(candPair.priority);
    s.mSelected.Construct(candPair.selected);
    s.mBytesSent.Construct(candPair.bytes_sent);
    s.mBytesReceived.Construct(candPair.bytes_recvd);
    s.mLastPacketSentTimestamp.Construct(candPair.ms_since_last_send);
    s.mLastPacketReceivedTimestamp.Construct(candPair.ms_since_last_recv);
    s.mState.Construct(RTCStatsIceCandidatePairState(candPair.state));
    s.mComponentId.Construct(candPair.component_id);
    report->mIceCandidatePairStats.Value().AppendElement(s, fallible);
  }

  std::vector<NrIceCandidate> candidates;
  if (NS_SUCCEEDED(aStream.GetLocalCandidates(&candidates))) {
    ToRTCIceCandidateStats(candidates,
                           RTCStatsType::Local_candidate,
                           transportId,
                           now,
                           report);
    // add the local candidates unparsed string to a sequence
    for (const auto& candidate : candidates) {
      report->mRawLocalCandidates.Value().AppendElement(
          NS_ConvertASCIItoUTF16(candidate.label.c_str()), fallible);
    }
  }
  candidates.clear();

  if (NS_SUCCEEDED(aStream.GetRemoteCandidates(&candidates))) {
    ToRTCIceCandidateStats(candidates,
                           RTCStatsType::Remote_candidate,
                           transportId,
                           now,
                           report);
    // add the remote candidates unparsed string to a sequence
    for (const auto& candidate : candidates) {
      report->mRawRemoteCandidates.Value().AppendElement(
          NS_ConvertASCIItoUTF16(candidate.label.c_str()), fallible);
    }
  }
}

nsresult
PeerConnectionMedia::AddTransceiver(
    JsepTransceiver* aJsepTransceiver,
    dom::MediaStreamTrack& aReceiveTrack,
    dom::MediaStreamTrack* aSendTrack,
    RefPtr<TransceiverImpl>* aTransceiverImpl)
{
  if (!mCall) {
    mCall = WebRtcCallWrapper::Create();
  }

  RefPtr<TransceiverImpl> transceiver = new TransceiverImpl(
      mParent->GetHandle(),
      aJsepTransceiver,
      mMainThread.get(),
      mSTSThread.get(),
      &aReceiveTrack,
      aSendTrack,
      mCall.get());

  if (!transceiver->IsValid()) {
    return NS_ERROR_FAILURE;
  }

  if (aSendTrack) {
    // implement checking for peerIdentity (where failure == black/silence)
    nsIDocument* doc = mParent->GetWindow()->GetExtantDoc();
    if (doc) {
      transceiver->UpdateSinkIdentity(nullptr,
                                      doc->NodePrincipal(),
                                      mParent->GetPeerIdentity());
    } else {
      MOZ_CRASH();
      return NS_ERROR_FAILURE; // Don't remove this till we know it's safe.
    }
  }

  mTransceivers.push_back(transceiver);
  *aTransceiverImpl = transceiver;

  return NS_OK;
}

void
PeerConnectionMedia::GetTransmitPipelinesMatching(
    const MediaStreamTrack* aTrack,
    nsTArray<RefPtr<MediaPipeline>>* aPipelines)
{
  for (RefPtr<TransceiverImpl>& transceiver : mTransceivers) {
    if (transceiver->HasSendTrack(aTrack)) {
      aPipelines->AppendElement(transceiver->GetSendPipeline());
    }
  }
}

void
PeerConnectionMedia::GetReceivePipelinesMatching(
    const MediaStreamTrack* aTrack,
    nsTArray<RefPtr<MediaPipeline>>* aPipelines)
{
  for (RefPtr<TransceiverImpl>& transceiver : mTransceivers) {
    if (transceiver->HasReceiveTrack(aTrack)) {
      aPipelines->AppendElement(transceiver->GetReceivePipeline());
    }
  }
}

std::string
PeerConnectionMedia::GetTransportIdMatching(
    const dom::MediaStreamTrack& aTrack) const
{
  for (const RefPtr<TransceiverImpl>& transceiver : mTransceivers) {
    if (transceiver->HasReceiveTrack(&aTrack)) {
      return transceiver->GetTransportId();
    }
  }
  return std::string();
}

nsresult
PeerConnectionMedia::AddRIDExtension(MediaStreamTrack& aRecvTrack,
                                     unsigned short aExtensionId)
{
  DebugOnly<bool> trackFound = false;
  for (RefPtr<TransceiverImpl>& transceiver : mTransceivers) {
    if (transceiver->HasReceiveTrack(&aRecvTrack)) {
      transceiver->AddRIDExtension(aExtensionId);
      trackFound = true;
    }
  }
  MOZ_ASSERT(trackFound);
  return NS_OK;
}

nsresult
PeerConnectionMedia::AddRIDFilter(MediaStreamTrack& aRecvTrack,
                                  const nsAString& aRid)
{
  DebugOnly<bool> trackFound = false;
  for (RefPtr<TransceiverImpl>& transceiver : mTransceivers) {
    MOZ_ASSERT(transceiver->HasReceiveTrack(&aRecvTrack));
    if (transceiver->HasReceiveTrack(&aRecvTrack)) {
      transceiver->AddRIDFilter(aRid);
      trackFound = true;
    }
  }
  MOZ_ASSERT(trackFound);
  return NS_OK;
}

void
PeerConnectionMedia::IceGatheringStateChange_s(NrIceCtx* ctx,
                                               NrIceCtx::GatheringState state)
{
  ASSERT_ON_THREAD(mSTSThread);

  if (state == NrIceCtx::ICE_CTX_GATHER_COMPLETE) {
    // Fire off EndOfLocalCandidates for each stream
    for (auto& stream : ctx->GetStreams()) {
      NrIceCandidate candidate;
      NrIceCandidate rtcpCandidate;
      GetDefaultCandidates(*stream, &candidate, &rtcpCandidate);
      EndOfLocalCandidates(candidate.cand_addr.host,
                           candidate.cand_addr.port,
                           rtcpCandidate.cand_addr.host,
                           rtcpCandidate.cand_addr.port,
                           stream->GetId());
    }
  }

  // ShutdownMediaTransport_s has not run yet because it unhooks this function
  // from its signal, which means that SelfDestruct_m has not been dispatched
  // yet either, so this PCMedia will still be around when this dispatch reaches
  // main.
  GetMainThread()->Dispatch(
    WrapRunnable(this,
                 &PeerConnectionMedia::IceGatheringStateChange_m,
                 ctx,
                 state),
    NS_DISPATCH_NORMAL);
}

void
PeerConnectionMedia::IceConnectionStateChange_s(NrIceCtx* ctx,
                                                NrIceCtx::ConnectionState state)
{
  ASSERT_ON_THREAD(mSTSThread);
  // ShutdownMediaTransport_s has not run yet because it unhooks this function
  // from its signal, which means that SelfDestruct_m has not been dispatched
  // yet either, so this PCMedia will still be around when this dispatch reaches
  // main.
  GetMainThread()->Dispatch(
    WrapRunnable(this,
                 &PeerConnectionMedia::IceConnectionStateChange_m,
                 ctx,
                 state),
    NS_DISPATCH_NORMAL);
}

void
PeerConnectionMedia::OnCandidateFound_s(NrIceMediaStream *aStream,
                                        const std::string &aCandidateLine)
{
  ASSERT_ON_THREAD(mSTSThread);
  MOZ_ASSERT(aStream);
  MOZ_ASSERT(!aStream->GetId().empty());
  MOZ_RELEASE_ASSERT(mIceCtx);

  CSFLogDebug(LOGTAG, "%s: %s", __FUNCTION__, aStream->name().c_str());

  NrIceCandidate candidate;
  NrIceCandidate rtcpCandidate;
  GetDefaultCandidates(*aStream, &candidate, &rtcpCandidate);

  // ShutdownMediaTransport_s has not run yet because it unhooks this function
  // from its signal, which means that SelfDestruct_m has not been dispatched
  // yet either, so this PCMedia will still be around when this dispatch reaches
  // main.
  GetMainThread()->Dispatch(
    WrapRunnable(this,
                 &PeerConnectionMedia::OnCandidateFound_m,
                 aCandidateLine,
                 candidate.cand_addr.host,
                 candidate.cand_addr.port,
                 rtcpCandidate.cand_addr.host,
                 rtcpCandidate.cand_addr.port,
                 aStream->GetId()),
    NS_DISPATCH_NORMAL);
}

void
PeerConnectionMedia::EndOfLocalCandidates(const std::string& aDefaultAddr,
                                          uint16_t aDefaultPort,
                                          const std::string& aDefaultRtcpAddr,
                                          uint16_t aDefaultRtcpPort,
                                          const std::string& aTransportId)
{
  GetMainThread()->Dispatch(
    WrapRunnable(this,
                 &PeerConnectionMedia::EndOfLocalCandidates_m,
                 aDefaultAddr,
                 aDefaultPort,
                 aDefaultRtcpAddr,
                 aDefaultRtcpPort,
                 aTransportId),
    NS_DISPATCH_NORMAL);
}

void
PeerConnectionMedia::GetDefaultCandidates(const NrIceMediaStream& aStream,
                                          NrIceCandidate* aCandidate,
                                          NrIceCandidate* aRtcpCandidate)
{
  nsresult res = aStream.GetDefaultCandidate(1, aCandidate);
  // Optional; component won't exist if doing rtcp-mux
  if (NS_FAILED(aStream.GetDefaultCandidate(2, aRtcpCandidate))) {
    aRtcpCandidate->cand_addr.host.clear();
    aRtcpCandidate->cand_addr.port = 0;
  }
  if (NS_FAILED(res)) {
    aCandidate->cand_addr.host.clear();
    aCandidate->cand_addr.port = 0;
    CSFLogError(LOGTAG, "%s: GetDefaultCandidates failed for transport id %s, "
                        "res=%u",
                        __FUNCTION__,
                        aStream.GetId().c_str(),
                        static_cast<unsigned>(res));
  }
}

static mozilla::dom::PCImplIceConnectionState
toDomIceConnectionState(NrIceCtx::ConnectionState state) {
  switch (state) {
    case NrIceCtx::ICE_CTX_INIT:
      return PCImplIceConnectionState::New;
    case NrIceCtx::ICE_CTX_CHECKING:
      return PCImplIceConnectionState::Checking;
    case NrIceCtx::ICE_CTX_CONNECTED:
      return PCImplIceConnectionState::Connected;
    case NrIceCtx::ICE_CTX_COMPLETED:
      return PCImplIceConnectionState::Completed;
    case NrIceCtx::ICE_CTX_FAILED:
      return PCImplIceConnectionState::Failed;
    case NrIceCtx::ICE_CTX_DISCONNECTED:
      return PCImplIceConnectionState::Disconnected;
    case NrIceCtx::ICE_CTX_CLOSED:
      return PCImplIceConnectionState::Closed;
  }
  MOZ_CRASH();
}

static mozilla::dom::PCImplIceGatheringState
toDomIceGatheringState(NrIceCtx::GatheringState state) {
  switch (state) {
    case NrIceCtx::ICE_CTX_GATHER_INIT:
      return PCImplIceGatheringState::New;
    case NrIceCtx::ICE_CTX_GATHER_STARTED:
      return PCImplIceGatheringState::Gathering;
    case NrIceCtx::ICE_CTX_GATHER_COMPLETE:
      return PCImplIceGatheringState::Complete;
  }
  MOZ_CRASH();
}

void
PeerConnectionMedia::IceGatheringStateChange_m(NrIceCtx* ctx,
                                               NrIceCtx::GatheringState state)
{
  ASSERT_ON_THREAD(mMainThread);
  SignalIceGatheringStateChange(toDomIceGatheringState(state));
}

void
PeerConnectionMedia::IceConnectionStateChange_m(NrIceCtx* ctx,
                                                NrIceCtx::ConnectionState state)
{
  ASSERT_ON_THREAD(mMainThread);
  SignalIceConnectionStateChange(toDomIceConnectionState(state));
}

void
PeerConnectionMedia::IceStreamReady_s(NrIceMediaStream *aStream)
{
  MOZ_ASSERT(aStream);

  CSFLogDebug(LOGTAG, "%s: %s", __FUNCTION__, aStream->name().c_str());
}

void
PeerConnectionMedia::OnCandidateFound_m(const std::string& aCandidateLine,
                                        const std::string& aDefaultAddr,
                                        uint16_t aDefaultPort,
                                        const std::string& aDefaultRtcpAddr,
                                        uint16_t aDefaultRtcpPort,
                                        const std::string& aTransportId)
{
  ASSERT_ON_THREAD(mMainThread);
  if (!aDefaultAddr.empty()) {
    SignalUpdateDefaultCandidate(aDefaultAddr,
                                 aDefaultPort,
                                 aDefaultRtcpAddr,
                                 aDefaultRtcpPort,
                                 aTransportId);
  }
  SignalCandidate(aCandidateLine, aTransportId);
}

void
PeerConnectionMedia::EndOfLocalCandidates_m(const std::string& aDefaultAddr,
                                            uint16_t aDefaultPort,
                                            const std::string& aDefaultRtcpAddr,
                                            uint16_t aDefaultRtcpPort,
                                            const std::string& aTransportId) {
  ASSERT_ON_THREAD(mMainThread);
  if (!aDefaultAddr.empty()) {
    SignalUpdateDefaultCandidate(aDefaultAddr,
                                 aDefaultPort,
                                 aDefaultRtcpAddr,
                                 aDefaultRtcpPort,
                                 aTransportId);
  }
  SignalEndOfLocalCandidates(aTransportId);
}

void
PeerConnectionMedia::DtlsConnected_s(TransportLayer *layer,
                                     TransportLayer::State state)
{
  MOZ_ASSERT(layer->id() == "dtls");
  TransportLayerDtls* dtlsLayer = static_cast<TransportLayerDtls*>(layer);
  dtlsLayer->SignalStateChange.disconnect(this);

  bool privacyRequested = (dtlsLayer->GetNegotiatedAlpn() == "c-webrtc");
  GetMainThread()->Dispatch(
    WrapRunnableNM(&PeerConnectionMedia::DtlsConnected_m,
                   mParentHandle, privacyRequested),
    NS_DISPATCH_NORMAL);
}

void
PeerConnectionMedia::DtlsConnected_m(const std::string& aParentHandle,
                                     bool aPrivacyRequested)
{
  PeerConnectionWrapper pcWrapper(aParentHandle);
  PeerConnectionImpl* pc = pcWrapper.impl();
  if (pc) {
    pc->SetDtlsConnected(aPrivacyRequested);
  }
}

void
PeerConnectionMedia::AddTransportFlow(const std::string& aId, bool aRtcp,
                                      const RefPtr<TransportFlow> &aFlow)
{
  auto& flows = aRtcp ? mRtcpTransportFlows : mTransportFlows;

  if (flows.count(aId)) {
    MOZ_ASSERT(false);
    return;
  }
  flows[aId] = aFlow;

  GetSTSThread()->Dispatch(
    WrapRunnable(this, &PeerConnectionMedia::ConnectDtlsListener_s, aFlow),
    NS_DISPATCH_NORMAL);
}

void
PeerConnectionMedia::RemoveTransportFlow(const std::string& aId, bool aRtcp)
{
  auto& flows = aRtcp ? mRtcpTransportFlows : mTransportFlows;
  auto it = flows.find(aId);
  if (it != flows.end()) {
    NS_ProxyRelease(
      "PeerConnectionMedia::mTransportFlows[aId] or mRtcpTransportFlows[aId]",
      GetSTSThread(), it->second.forget());
    flows.erase(it);
  }
}

void
PeerConnectionMedia::ConnectDtlsListener_s(const RefPtr<TransportFlow>& aFlow)
{
  TransportLayer* dtls = aFlow->GetLayer(TransportLayerDtls::ID());
  if (dtls) {
    dtls->SignalStateChange.connect(this, &PeerConnectionMedia::DtlsConnected_s);
  }
}

/**
 * Tells you if any local track is isolated to a specific peer identity.
 * Obviously, we want all the tracks to be isolated equally so that they can
 * all be sent or not.  We check once when we are setting a local description
 * and that determines if we flip the "privacy requested" bit on.  Once the bit
 * is on, all media originating from this peer connection is isolated.
 *
 * @returns true if any track has a peerIdentity set on it
 */
bool
PeerConnectionMedia::AnyLocalTrackHasPeerIdentity() const
{
  ASSERT_ON_THREAD(mMainThread);

  for (const RefPtr<TransceiverImpl>& transceiver : mTransceivers) {
    if (transceiver->GetSendTrack() &&
        transceiver->GetSendTrack()->GetPeerIdentity()) {
      return true;
    }
  }
  return false;
}

void
PeerConnectionMedia::UpdateRemoteStreamPrincipals_m(nsIPrincipal* aPrincipal)
{
  ASSERT_ON_THREAD(mMainThread);

  for (RefPtr<TransceiverImpl>& transceiver : mTransceivers) {
    transceiver->UpdatePrincipal(aPrincipal);
  }
}

void
PeerConnectionMedia::UpdateSinkIdentity_m(const MediaStreamTrack* aTrack,
                                          nsIPrincipal* aPrincipal,
                                          const PeerIdentity* aSinkIdentity)
{
  ASSERT_ON_THREAD(mMainThread);

  for (RefPtr<TransceiverImpl>& transceiver : mTransceivers) {
    transceiver->UpdateSinkIdentity(aTrack, aPrincipal, aSinkIdentity);
  }
}

bool
PeerConnectionMedia::AnyCodecHasPluginID(uint64_t aPluginID)
{
  for (RefPtr<TransceiverImpl>& transceiver : mTransceivers) {
    if (transceiver->ConduitHasPluginID(aPluginID)) {
      return true;
    }
  }
  return false;
}

} // namespace mozilla
