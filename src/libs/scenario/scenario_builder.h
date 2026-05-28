#ifndef SCRATCH_SCENARIO_BUILDER_H
#define SCRATCH_SCENARIO_BUILDER_H

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "controller/zmq-openflow-controller.h"
#include "ns3/csma-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/ofswitch13-module.h"
#include "scenario/scenario_stress.h"
#include "scenario/scenario_topo.h"

namespace ns3 {

// Replaces both UsaTopologyBuilder and MultiCtrlBuilder. Single-controller
// scenarios call InstallOpenFlow with one ctrl + the full switch list;
// multi-controller scenarios pass M ctrls + per-section switch lists.
class ScenarioBuilder {
 public:
  ScenarioBuilder();

  void CreateNodes(uint32_t numHosts, uint32_t numSwitches,
                   uint32_t numControllers);

  // Returns the 1-indexed OF port the host attaches to on switchIdx.
  uint32_t InstallHost(uint32_t hostIdx, uint32_t switchIdx,
                       const std::string& edgeQueueSize);

  // Returns (srcPort, dstPort) in 1-indexed OF port space and tracks the
  // link in failureLinks storage when spec.failureTarget is true.
  std::pair<uint32_t, uint32_t> AddBackboneLink(
      const LinkSpec& spec, const std::string& backboneQueueSize);

  void SetupIpStack();
  void PrePopulateArp();

  // ctrls[i] owns the switches whose indices are in switchesPerCtrl[i].
  // For single-controller mode, pass {ctrl} and {{0,1,...,numSwitches-1}}.
  void InstallOpenFlow(
      const std::vector<Ptr<ZmqOpenFlowController>>& ctrls,
      const std::vector<std::vector<uint32_t>>& switchesPerCtrl);

  void ConfigureSwitch(uint32_t idx, const NodeProfile& profile,
                       Ptr<ZmqOpenFlowController> ctrl);
  void EnableTraces(const std::string& prefix);

  NodeContainer& GetHosts() { return m_hosts; }
  NodeContainer& GetSwitches() { return m_switches; }
  Ipv4InterfaceContainer& GetHostIfaces() { return m_hostIfaces; }
  Mac48Address HostMac(uint32_t hostIdx) const;
  // Returns the 1-indexed OF port on srcDpid that egresses toward dstDpid,
  // or 0 if no such direct link exists.
  uint32_t PortBetween(uint64_t srcDpid, uint64_t dstDpid) const;

  // HostInfo for every host installed, in install order. {mac, dpid, ofPort}.
  const std::vector<ZmqOpenFlowController::HostInfo>& GetHostInfos() const {
    return m_hostMetas;
  }
  std::vector<LinkController::State*> GetFailureLinks();

 private:
  static uint64_t MacToU64(const Mac48Address& addr);
  void ConfigureQueue(Ptr<NetDevice> nd, const std::string& sizeStr);
  void SetLinkErrorRate(Ptr<NetDevice> nd, double rate);

  uint32_t m_numHosts = 0;
  uint32_t m_numSwitches = 0;
  uint32_t m_numControllers = 0;
  NodeContainer m_hosts;
  NodeContainer m_switches;
  NodeContainer m_controllers;
  NetDeviceContainer m_hostPorts;
  std::vector<NetDeviceContainer> m_swPorts;
  Ipv4InterfaceContainer m_hostIfaces;
  std::vector<Ptr<OFSwitch13InternalHelper>> m_helpers;
  CsmaHelper m_edgeHelper;
  CsmaHelper m_backboneHelper;
  std::vector<LinkController::State> m_linkStorage;
  std::vector<ZmqOpenFlowController::HostInfo> m_hostMetas;
  // Maps (srcDpid, dstDpid) → 1-indexed OF port on srcDpid.
  std::map<std::pair<uint64_t, uint64_t>, uint32_t> m_portBetween;
};

}  // namespace ns3

#endif  // SCRATCH_SCENARIO_BUILDER_H
