#include "scenario/scenario_builder.h"

#include <iomanip>
#include <sstream>

#include "ns3/core-module.h"
#include "ns3/error-model.h"
#include "ns3/log.h"
#include "ns3/queue.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("ScenarioBuilder");

ScenarioBuilder::ScenarioBuilder() {
  m_edgeHelper.SetChannelAttribute("DataRate", StringValue("100Mbps"));
  m_edgeHelper.SetChannelAttribute("Delay", StringValue("1ms"));
  m_edgeHelper.SetChannelAttribute("FullDuplex", BooleanValue(true));
  m_edgeHelper.SetDeviceAttribute("Mtu", UintegerValue(9000));
  m_backboneHelper.SetChannelAttribute("DataRate", StringValue("1Gbps"));
  m_backboneHelper.SetChannelAttribute("FullDuplex", BooleanValue(true));
  m_backboneHelper.SetDeviceAttribute("Mtu", UintegerValue(9000));
}

void ScenarioBuilder::CreateNodes(uint32_t numHosts, uint32_t numSwitches,
                                  uint32_t numControllers) {
  m_numHosts = numHosts;
  m_numSwitches = numSwitches;
  m_numControllers = numControllers;
  m_hosts.Create(numHosts);
  m_switches.Create(numSwitches);
  m_controllers.Create(numControllers);
  m_swPorts.resize(numSwitches);
  m_linkStorage.reserve(16);
}

uint32_t ScenarioBuilder::InstallHost(uint32_t hostIdx, uint32_t switchIdx,
                                      const std::string& edgeQueueSize) {
  NetDeviceContainer dev = m_edgeHelper.Install(
      NodeContainer(m_hosts.Get(hostIdx), m_switches.Get(switchIdx)));
  m_hostPorts.Add(dev.Get(0));
  m_swPorts[switchIdx].Add(dev.Get(1));
  uint32_t ofPort = m_swPorts[switchIdx].GetN();  // 1-indexed
  ConfigureQueue(dev.Get(1), edgeQueueSize);

  Mac48Address addr = Mac48Address::ConvertFrom(
      m_hosts.Get(hostIdx)->GetDevice(0)->GetAddress());
  m_hostMetas.push_back({MacToU64(addr), switchIdx + 1, ofPort});
  return ofPort;
}

std::pair<uint32_t, uint32_t> ScenarioBuilder::AddBackboneLink(
    const LinkSpec& spec, const std::string& backboneQueueSize) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(6) << (spec.distanceKm * 5e-6) << "s";
  m_backboneHelper.SetChannelAttribute("Delay", StringValue(oss.str()));
  m_backboneHelper.SetChannelAttribute("DataRate", StringValue(spec.dataRate));

  NetDeviceContainer dev = m_backboneHelper.Install(
      NodeContainer(m_switches.Get(spec.src), m_switches.Get(spec.dst)));
  m_swPorts[spec.src].Add(dev.Get(0));
  m_swPorts[spec.dst].Add(dev.Get(1));
  uint32_t srcPort = m_swPorts[spec.src].GetN();
  uint32_t dstPort = m_swPorts[spec.dst].GetN();

  ConfigureQueue(dev.Get(0), backboneQueueSize);
  ConfigureQueue(dev.Get(1), backboneQueueSize);

  if (spec.lossRate > 0.0) {
    SetLinkErrorRate(dev.Get(0), spec.lossRate);
    SetLinkErrorRate(dev.Get(1), spec.lossRate);
  }

  uint64_t srcDpid = spec.src + 1;
  uint64_t dstDpid = spec.dst + 1;
  m_portBetween[{srcDpid, dstDpid}] = srcPort;
  m_portBetween[{dstDpid, srcDpid}] = dstPort;

  if (spec.failureTarget) {
    LinkController::State st;
    st.devA = dev.Get(0);
    st.devB = dev.Get(1);
    st.normalRate = spec.lossRate;
    m_linkStorage.push_back(st);
  }
  return {srcPort, dstPort};
}

void ScenarioBuilder::SetupIpStack() {
  InternetStackHelper internet;
  internet.Install(m_hosts);
  Ipv4AddressHelper ipv4;
  ipv4.SetBase("10.1.1.0", "255.255.255.0");
  m_hostIfaces = ipv4.Assign(m_hostPorts);
}

void ScenarioBuilder::PrePopulateArp() {
  uint32_t n = m_numHosts;
  for (uint32_t i = 0; i < n; ++i) {
    Ptr<Node> host = m_hosts.Get(i);
    Ptr<Ipv4L3Protocol> ip = host->GetObject<Ipv4L3Protocol>();
    Ptr<ArpCache> cache = CreateObject<ArpCache>();
    cache->SetAliveTimeout(Seconds(86400));

    for (uint32_t j = 0; j < n; ++j) {
      if (i == j) continue;
      Ipv4Address addr = m_hostIfaces.GetAddress(j);
      Address mac = m_hosts.Get(j)->GetDevice(0)->GetAddress();
      ArpCache::Entry* entry = cache->Add(addr);
      entry->SetMacAddress(Mac48Address::ConvertFrom(mac));
      entry->MarkPermanent();
    }
    // iface 0 = loopback, 1 = edge link
    ip->GetInterface(1)->SetArpCache(cache);
  }
}

void ScenarioBuilder::InstallOpenFlow(
    const std::vector<Ptr<ZmqOpenFlowController>>& ctrls,
    const std::vector<std::vector<uint32_t>>& switchesPerCtrl) {
  NS_ASSERT(ctrls.size() == switchesPerCtrl.size());
  NS_ASSERT(ctrls.size() == m_numControllers);
  m_helpers.clear();
  m_helpers.resize(ctrls.size());
  for (uint32_t c = 0; c < ctrls.size(); ++c) {
    m_helpers[c] = CreateObject<OFSwitch13InternalHelper>();
    m_helpers[c]->SetChannelType(OFSwitch13Helper::DEDICATED_P2P);
    m_controllers.Get(c)->AddApplication(ctrls[c]);
    ctrls[c]->SetStartTime(Seconds(0));
    m_helpers[c]->InstallController(m_controllers.Get(c), ctrls[c]);
    for (uint32_t sw : switchesPerCtrl[c]) {
      if (sw >= m_numSwitches) continue;
      m_helpers[c]->InstallSwitch(m_switches.Get(sw), m_swPorts[sw]);
    }
    m_helpers[c]->CreateOpenFlowChannels();
  }
}

void ScenarioBuilder::ConfigureSwitch(uint32_t idx, const NodeProfile& profile,
                                      Ptr<ZmqOpenFlowController> ctrl) {
  Ptr<OFSwitch13Device> ofDev =
      m_switches.Get(idx)->GetObject<OFSwitch13Device>();
  if (!ofDev) return;

  ofDev->SetAttribute("CpuCapacity",
                      DataRateValue(DataRate(profile.cpuCapacity)));
  ofDev->SetAttribute("TcamDelay",
                      TimeValue(MicroSeconds(profile.tcamDelayUs)));

  uint64_t dpid = idx + 1;
  ctrl->SetSwitchEnergyModel(dpid, profile.initialEnergyJ,
                             profile.energyPerByteJ, profile.idlePowerW);
}

void ScenarioBuilder::EnableTraces(const std::string& prefix) {
  for (auto& h : m_helpers) {
    if (!h) continue;
    h->EnableOpenFlowPcap(prefix);
    h->EnableDatapathStats(prefix + "-stats");
  }
}

Mac48Address ScenarioBuilder::HostMac(uint32_t hostIdx) const {
  Ptr<NetDevice> nd = m_hosts.Get(hostIdx)->GetDevice(0);
  return Mac48Address::ConvertFrom(nd->GetAddress());
}

uint32_t ScenarioBuilder::PortBetween(uint64_t srcDpid, uint64_t dstDpid) const {
  auto it = m_portBetween.find({srcDpid, dstDpid});
  return it == m_portBetween.end() ? 0 : it->second;
}

std::vector<LinkController::State*> ScenarioBuilder::GetFailureLinks() {
  std::vector<LinkController::State*> out;
  out.reserve(m_linkStorage.size());
  for (auto& s : m_linkStorage) out.push_back(&s);
  return out;
}

uint64_t ScenarioBuilder::MacToU64(const Mac48Address& addr) {
  uint8_t buf[6];
  addr.CopyTo(buf);
  uint64_t v = 0;
  for (int i = 0; i < 6; ++i) v = (v << 8) | buf[i];
  return v;
}

void ScenarioBuilder::ConfigureQueue(Ptr<NetDevice> nd,
                                     const std::string& sizeStr) {
  Ptr<CsmaNetDevice> csma = DynamicCast<CsmaNetDevice>(nd);
  if (!csma) return;
  Ptr<Queue<Packet>> q = csma->GetQueue();
  if (q) {
    q->SetAttribute("MaxSize", QueueSizeValue(QueueSize(sizeStr)));
  }
}

void ScenarioBuilder::SetLinkErrorRate(Ptr<NetDevice> nd, double rate) {
  Ptr<CsmaNetDevice> csma = DynamicCast<CsmaNetDevice>(nd);
  if (!csma) return;
  Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
  em->SetAttribute("ErrorRate", DoubleValue(rate));
  em->SetAttribute("ErrorUnit", EnumValue(RateErrorModel::ERROR_UNIT_PACKET));
  csma->SetAttribute("ReceiveErrorModel", PointerValue(em));
}

}  // namespace ns3
