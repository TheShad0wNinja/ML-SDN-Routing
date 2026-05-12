#!/usr/bin/env python3
"""
SDN Topology Visualizer
Reads controller state JSON and writes an interactive, self-contained HTML file.
Usage:
    python topology-viewer.py [--input PATH] [--output PATH] [--watch]
"""
import argparse
import json
import os
import time
from datetime import datetime

_SCRATCH_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_DEFAULT_STATE = os.path.join(_SCRATCH_ROOT, "data", "state", "sdn_state.json")
_DEFAULT_HTML  = os.path.join(_SCRATCH_ROOT, "data", "html",  "topology.html")

# Palette for controller nodes — border color cycles through this list by index
CONTROLLER_COLORS = [
    {"bg": "#fbbf24", "border": "#d97706"},  # amber
    {"bg": "#f472b6", "border": "#db2777"},  # pink
    {"bg": "#34d399", "border": "#059669"},  # green
    {"bg": "#818cf8", "border": "#4f46e5"},  # indigo
    {"bg": "#fb923c", "border": "#ea580c"},  # orange
    {"bg": "#22d3ee", "border": "#0891b2"},  # cyan
]

# ---------------------------------------------------------------------------
# HTML template
# ---------------------------------------------------------------------------
HTML_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>SDN Topology</title>
<script src="https://cdnjs.cloudflare.com/ajax/libs/vis/4.21.0/vis.min.js"></script>
<link href="https://cdnjs.cloudflare.com/ajax/libs/vis/4.21.0/vis.min.css" rel="stylesheet" />
<style>
body { margin:0; font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
       background:#0f172a; color:#e2e8f0; }
#network { width:100vw; height:100vh; }
#panel {
  position:absolute; top:12px; right:12px; width:360px;
  background:rgba(15,23,42,0.97); border:1px solid #334155;
  padding:16px; border-radius:10px; max-height:90vh; overflow-y:auto;
  font-size:13px; box-shadow:0 10px 30px rgba(0,0,0,.5); z-index:10;
}
h3 { margin-top:0; color:#38bdf8; }
h4 { color:#94a3b8; margin:10px 0 4px; }
p  { margin:4px 0; }
table { width:100%; border-collapse:collapse; margin-top:6px; font-size:12px; }
th,td { text-align:left; padding:4px 5px; border-bottom:1px solid #334155; }
th { color:#94a3b8; font-weight:600; }
.metric-row { display:flex; justify-content:space-between; padding:3px 0;
              border-bottom:1px solid #1e293b; font-size:12px; }
.metric-label { color:#94a3b8; }
.metric-value { color:#e2e8f0; font-variant-numeric:tabular-nums; }
.metric-value.warn  { color:#fbbf24; }
.metric-value.crit  { color:#ef4444; }
.energy-bar-wrap { background:#1e293b; border-radius:4px; height:8px;
                   margin:4px 0 8px; overflow:hidden; }
.energy-bar { height:100%; border-radius:4px; transition:width .3s; }
.legend {
  position:absolute; bottom:12px; left:12px;
  background:rgba(15,23,42,0.95); border:1px solid #334155;
  padding:10px 14px; border-radius:8px; font-size:12px; z-index:10;
}
.legend-item { display:flex; align-items:center; margin:5px 0; }
.legend-box  { width:14px; height:14px; margin-right:8px; border-radius:3px; }
.legend-dot  { width:12px; height:12px; border-radius:50%; margin-right:9px; margin-left:1px; }
.timestamp {
  position:absolute; top:12px; left:12px;
  background:rgba(15,23,42,0.95); border:1px solid #334155;
  padding:8px 12px; border-radius:8px; font-size:12px; color:#94a3b8; z-index:10;
}
</style>
</head>
<body>
<div id="network"></div>
<div class="timestamp">Last updated: TIMESTAMP_PLACEHOLDER</div>
<div class="legend">
  CTRL_LEGEND_PLACEHOLDER
  <div class="legend-item" style="font-size:11px;color:#64748b;margin:-2px 0 4px">(Switch border = controller)</div>
  <div class="legend-item"><div class="legend-box" style="background:#38bdf8"></div>Switch</div>
  <div class="legend-item"><div class="legend-dot" style="background:#f472b6"></div>Host</div>
  <div class="legend-item"><div class="legend-dot" style="background:#a78bfa"></div>IoT Sensor</div>
  <div class="legend-item"><div class="legend-dot" style="background:#34d399"></div>IoT Gateway</div>
  <hr style="border-color:#334155;margin:6px 0;">
  <div class="legend-item"><span style="color:#22c55e">─────</span>&nbsp;Link &lt;50% load</div>
  <div class="legend-item"><span style="color:#f59e0b">─────</span>&nbsp;Link 50-80% load</div>
  <div class="legend-item"><span style="color:#ef4444">─────</span>&nbsp;Link &gt;80% load</div>
</div>
<div id="panel">
  <h3>Network Overview</h3>
  <p><b>Switches:</b> <span id="sw-count">0</span></p>
  <p><b>Hosts:</b> <span id="host-count">0</span></p>
  <p><b>Links:</b> <span id="link-count">0</span></p>
  <div id="global-stats"></div>
  <hr style="border-color:#334155;margin:12px 0;">
  <p style="color:#64748b;">Click the controller, a switch, or a host for details.</p>
</div>

<script>
const nodeData  = NODES_JSON_PLACEHOLDER;
const edgeData  = EDGES_JSON_PLACEHOLDER;
const statsData = STATS_JSON_PLACEHOLDER;
const switchObs = SWITCH_OBS_PLACEHOLDER;
const atvmData  = ATVM_JSON_PLACEHOLDER;
const muMaxGlobal = MU_MAX_GLOBAL_PLACEHOLDER;

// ---- helper formatters ----
function fmtBps(bps) {
  if (bps >= 1e9) return (bps/1e9).toFixed(2)+' Gbps';
  if (bps >= 1e6) return (bps/1e6).toFixed(2)+' Mbps';
  if (bps >= 1e3) return (bps/1e3).toFixed(1)+' kbps';
  return bps.toFixed(0)+' bps';
}
function fmtBytes(b) {
  if (b >= 1e9) return (b/1e9).toFixed(2)+' GB';
  if (b >= 1e6) return (b/1e6).toFixed(2)+' MB';
  if (b >= 1e3) return (b/1e3).toFixed(1)+' kB';
  return b+' B';
}
function rhoClass(rho) {
  if (rho >= 0.8) return 'crit';
  if (rho >= 0.5) return 'warn';
  return '';
}
function metricRow(label, value, cls='') {
  return `<div class="metric-row">
    <span class="metric-label">${label}</span>
    <span class="metric-value ${cls}">${value}</span>
  </div>`;
}

const nodes = new vis.DataSet(nodeData);
const edges = new vis.DataSet(edgeData);
const container = document.getElementById('network');
const options = {
  groups: {
    switch: {
      shape:'box',
      font:{color:'#0f172a',face:'sans-serif',size:14}, margin:10,
      shadow:{enabled:true,color:'rgba(0,0,0,.5)',size:10}
    },
    host: {
      shape:'dot', color:{background:'#f472b6',border:'#db2777',
        highlight:{background:'#f9a8d4',border:'#f472b6'}},
      font:{color:'#f8fafc',face:'sans-serif',size:14,bold:true,multi:true},
      size:22, shadow:{enabled:true,color:'rgba(0,0,0,.5)',size:12}
    },
    iot_sensor: {
      shape:'dot', color:{background:'#a78bfa',border:'#7c3aed',
        highlight:{background:'#c4b5fd',border:'#a78bfa'}},
      font:{color:'#f8fafc',face:'sans-serif',size:13,bold:true,multi:true},
      size:20, shadow:{enabled:true,color:'rgba(0,0,0,.5)',size:10}
    },
    iot_gateway: {
      shape:'dot', color:{background:'#34d399',border:'#059669',
        highlight:{background:'#6ee7b7',border:'#34d399'}},
      font:{color:'#f8fafc',face:'sans-serif',size:13,bold:true,multi:true},
      size:22, shadow:{enabled:true,color:'rgba(0,0,0,.5)',size:10}
    }
  },
  edges:{
    width:2,
    color:{color:'#475569',highlight:'#38bdf8',hover:'#94a3b8'},
    smooth:{type:'dynamic'},
    font:{color:'#fff',size:12,align:'middle',
          background:{enabled:true,color:'rgba(15,23,42,.75)'}}
  },
  physics:{
    stabilization:{iterations:250},
    barnesHut:{gravitationalConstant:-6000,centralGravity:0.2,
               springLength:220,springConstant:0.04}
  },
  interaction:{hover:true,tooltipDelay:150}
};
const network = new vis.Network(container, {nodes, edges}, options);

network.once('stabilizationIterationsDone', function() {
  const hostIds = nodes.get({filter:n=>n.group==='host'||n.group==='iot_sensor'||n.group==='iot_gateway'}).map(n=>n.id);
  const focus = new Set();
  hostIds.forEach(hid=>{focus.add(hid); network.getConnectedNodes(hid).forEach(c=>focus.add(c));});
  nodes.get({filter:n=>n.group==='controller'}).forEach(n=>{
    focus.add(n.id); network.getConnectedNodes(n.id).forEach(c=>focus.add(c));});
  if (focus.size>0) network.fit({nodes:Array.from(focus),animation:{duration:400,easingFunction:'easeInOutQuad'},padding:80});
});

document.getElementById('sw-count').textContent   = nodes.get({filter:n=>n.group==='switch'}).length;
document.getElementById('host-count').textContent = nodes.get({filter:n=>['host','iot_sensor','iot_gateway'].includes(n.group)}).length;
document.getElementById('link-count').textContent = edges.length;

// ---- global stats panel ----
(function renderGlobal(){
  const panel = document.getElementById('global-stats');
  if (!switchObs || Object.keys(switchObs).length===0) return;
  let totalRho=0, nRho=0, maxLambda=0;
  for (const o of Object.values(switchObs)) {
    if (o.rho != null) { totalRho += o.rho; nRho++; }
    if ((o.lambda_bps||0) > maxLambda) maxLambda = o.lambda_bps;
  }
  const avgRho = nRho > 0 ? totalRho/nRho : null;
  panel.innerHTML =
    '<hr style="border-color:#334155;margin:10px 0;">'+
    '<h4 style="margin-top:0">Network-Wide State</h4>'+
    (avgRho != null ? metricRow('Avg ρ (utilisation)', (avgRho*100).toFixed(1)+'%', rhoClass(avgRho)) : '')+
    metricRow('Peak λ (arrival)',    fmtBps(maxLambda))+
    (muMaxGlobal>0 ? metricRow('μ_max (global cap)', fmtBps(muMaxGlobal)) : '');
})();

// ---- click handler ----
network.on('click', function(params) {
  const panel = document.getElementById('panel');
  if (params.nodes.length===0 && params.edges.length===0) {
    panel.innerHTML = `
      <h3>Network Overview</h3>
      <p><b>Switches:</b> ${nodes.get({filter:n=>n.group==='switch'}).length}</p>
      <p><b>Hosts:</b> ${nodes.get({filter:n=>['host','iot_sensor','iot_gateway'].includes(n.group)}).length}</p>
      <p><b>Links:</b> ${edges.length}</p>
      <hr style="border-color:#334155;margin:12px 0;">
      <p style="color:#64748b;">Click the controller, a switch, or a host for details.</p>`;
    return;
  } else if (params.nodes.length === 0 && params.edges.length > 0) {
    const edgeId = params.edges[0];
    const edge = edges.get(edgeId);
    let html = '<h3>Link Details</h3>';
    if (edge.is_host_link) {
      html += '<p><b>Type:</b> Host ↔ Switch</p>';
      html += '<p><b>Host MAC:</b> ' + edge.mac + '</p>';
      html += '<p><b>Switch DPID:</b> ' + edge.sw + ' &nbsp;<b>Port:</b> ' + edge.port + '</p>';
    } else {
      html += '<p><b>Type:</b> Switch ↔ Switch</p>';
      html += '<p><b>Connection:</b> Dpid: ' + edge.src_dpid + ' (port ' + edge.src_port + ') ↔ Dpid ' + edge.dst_dpid + ' (port ' + edge.dst_port + ')</p>';
      html += '<p><b>Cost:</b> ' + edge.cost_ms + ' ms</p>';
      html += '<h4>Bandwidth & Utilization</h4>';
      html += metricRow('Capacity', fmtBps(edge.speed_bps));
      html += metricRow('Traffic dpid ' + edge.src_dpid + ' → dpid ' + edge.dst_dpid, fmtBps(edge.rate_ab));
      html += metricRow('Traffic dpid ' + edge.dst_dpid + ' → dpid ' + edge.src_dpid, fmtBps(edge.rate_ba));
      if (edge.speed_bps > 0) {
          html += metricRow('Peak Utilization', (edge.util * 100).toFixed(2) + '%', rhoClass(edge.util));
      }
    }
    panel.innerHTML = html;
    return;
  }

  const nodeId = params.nodes[0];
  const node   = nodes.get(nodeId);
  let html = '<h3>'+(node.label||nodeId).replace(/\\n/g,' ')+'</h3>';
  html += '<p><b>Type:</b> <span style="text-transform:capitalize">'+node.group+'</span></p>';

  // ── Switch ───────────────────────────────────────────────────────────
  if (node.group==='switch') {
    html += '<p><b>DPID:</b> '+nodeId+'</p>';
    if (node.ctrlLabel) {
      const cc = node.ctrlBorder||'#d97706';
      html += '<p><b>Controller:</b> <span style="color:'+cc+';font-weight:600">'+node.ctrlLabel+'</span></p>';
    }

    // Per-port stats (read early so obs section can use port speeds)
    const swStats = statsData[String(nodeId)];

    // DDPG observation vector
    const obs = switchObs[String(nodeId)];
    if (obs) {
      html += '<h4>Switch Observation (M/M/1/K)</h4>';
      html += metricRow('λ (arrival rate)',  fmtBps(obs.lambda_bps || 0));
      if (obs.mu_max_bps != null) html += metricRow('μ_max (capacity)', fmtBps(obs.mu_max_bps));
      if (obs.rho        != null) html += metricRow('ρ (utilisation)',  (obs.rho*100).toFixed(2)+'%', rhoClass(obs.rho));
      html += metricRow('K (queue cap)', (obs.K||0).toFixed(0)+' pkts');
      if (obs.N    != null) html += metricRow('N (queue depth)', obs.N.toFixed(3)+' pkts', obs.N>32?'crit':obs.N>10?'warn':'');
      if (obs.d_ms   != null) html += metricRow('d (delay est.)',   obs.d_ms.toFixed(3)+' ms',          obs.d_ms>50?'crit':obs.d_ms>10?'warn':'');
      if (obs.p_loss != null) html += metricRow('P_loss (blocking)',(obs.p_loss*100).toFixed(4)+'%',   obs.p_loss>0.05?'crit':obs.p_loss>0.01?'warn':'');
      html += metricRow('L (loss rate)', fmtBps(obs.L_bps || 0));
      if (obs.rbw_bps != null) html += metricRow('RBW', fmtBps(obs.rbw_bps));

      if (obs.residual_energy_j != null && obs.residual_energy_j >= 0) {
        const ej = obs.residual_energy_j;
        const barColor = ej > 5000 ? '#22c55e' : ej > 1000 ? '#f59e0b' : '#ef4444';
        const pct = Math.min(100, (ej / 10000) * 100);
        html += '<h4>Switch Energy</h4>';
        html += '<div style="margin:6px 0 2px"><span class="metric-label">Residual energy</span></div>';
        html += '<div class="energy-bar-wrap"><div class="energy-bar" style="width:'+pct.toFixed(1)+'%;background:'+barColor+'"></div></div>';
        html += metricRow('', ej.toFixed(1)+' J remaining');
      }
    }
    if (swStats && Object.keys(swStats).length>0) {
      html += '<h4>Port Statistics</h4>';
      html += '<table><tr><th>Port</th><th>RX</th><th>TX</th><th>RX rate</th><th>TX rate</th><th>Drop</th></tr>';
      for (const [pno, s] of Object.entries(swStats)) {
        html += '<tr>';
        html += '<td>'+pno+'</td>';
        html += '<td>'+fmtBytes(s.rx_bytes??0)+'</td>';
        html += '<td>'+fmtBytes(s.tx_bytes??0)+'</td>';
        html += '<td>'+(s.rx_rate_bps!=null?fmtBps(s.rx_rate_bps):'—')+'</td>';
        html += '<td>'+(s.tx_rate_bps!=null?fmtBps(s.tx_rate_bps):'—')+'</td>';
        html += '<td>'+((s.rx_dropped??0)+(s.tx_dropped??0))+'</td>';
        html += '</tr>';
      }
      html += '</table>';
    } else {
      html += '<p style="color:#64748b">No port stats yet (wait for first collection cycle).</p>';
    }

  // ── Host / IoT ───────────────────────────────────────────────────────
  } else if (['host','iot_sensor','iot_gateway'].includes(node.group)) {
    if (node.name) html += '<p><b>Name:</b> <span style="color:#fbbf24;font-size:1.1em">'+node.name+'</span></p>';
    html += '<p><b>IP:</b> '+(node.ip
      ? '<span style="color:#7dd3fc;font-size:1.1em">'+node.ip+'</span>'
      : '<span style="color:#64748b">unknown</span>')+'</p>';
    html += '<p><b>MAC:</b> '+nodeId+'</p>';
    html += '<p><b>Switch:</b> '+(node.dpid||'?')+' &nbsp;<b>Port:</b> '+(node.port||'?')+'</p>';

  }
  panel.innerHTML = html;
});
</script>
</body>
</html>
"""


def _utilisation_color(util_fraction: float) -> str:
    """Return a hex color for a link utilisation fraction 0-1."""
    if util_fraction >= 0.8:
        return "#ef4444"  # red
    if util_fraction >= 0.5:
        return "#f59e0b"  # amber
    return "#22c55e"      # green


def _link_width(util_fraction: float) -> float:
    """Scale edge width 1..8 based on utilisation."""
    return 1.0 + util_fraction * 7.0


def _fmt_bps(bps: float) -> str:
    if bps >= 1e9:  return f"{bps/1e9:.2f} Gbps"
    if bps >= 1e6:  return f"{bps/1e6:.2f} Mbps"
    if bps >= 1e3:  return f"{bps/1e3:.1f} kbps"
    return f"{bps:.0f} bps"


def generate_html(state: dict, output_path: str) -> None:
    switches      = state.get("switches", [])
    hosts         = state.get("hosts",    [])
    links         = state.get("links",    [])
    stats         = state.get("stats",    {})
    switch_obs    = state.get("switch_observations", {})
    atvm          = state.get("atvm",     [])
    mu_max_global = state.get("mu_max_global_bps", 0)
    control_links = state.get("control_links")
    if control_links is None:
        control_links = []
        for sw in switches:
            dpid = sw["dpid"] if isinstance(sw, dict) else sw
            control_links.append({"dpid": dpid})

    # ------------------------------------------------------------------
    # Controller palette — support both new `controllers` array and legacy
    # singular `controller` key.
    # ------------------------------------------------------------------
    raw_ctrls = state.get("controllers") or []
    if not raw_ctrls and state.get("controller"):
        raw_ctrls = [state["controller"]]

    controllers_colored = []
    for i, c in enumerate(raw_ctrls):
        color = CONTROLLER_COLORS[i % len(CONTROLLER_COLORS)]
        controllers_colored.append({**c, "_color": color})

    default_color = (controllers_colored[0]["_color"]
                     if controllers_colored else CONTROLLER_COLORS[0])

    # Map dpid (int) → color dict, using optional controller_id on each link
    dpid_to_ctrl_color: dict = {}
    for cl in control_links:
        dpid = cl.get("dpid")
        if dpid is None:
            continue
        ctrl_id = cl.get("controller_id")
        color = default_color
        if ctrl_id:
            for c in controllers_colored:
                if c.get("id") == ctrl_id:
                    color = c["_color"]
                    break
        dpid_to_ctrl_color[dpid] = color

    # Legend HTML fragment — one diamond per controller
    ctrl_legend_html = ""
    for c in controllers_colored:
        col   = c["_color"]
        label = c.get("label", "Controller")
        ctrl_legend_html += (
            f'<div class="legend-item">'
            f'<div class="legend-box" style="background:{col["border"]};'
            f'transform:rotate(45deg);width:12px;height:12px;border-radius:2px">'
            f'</div>{label}</div>\n  '
        )

    # Build ATVM lookup: (src_dpid, dst_dpid) -> tx_bps
    atvm_map: dict = {}
    for entry in atvm:
        atvm_map[(entry["src"], entry["dst"])] = entry.get("tx_bps", 0)

    # ------------------------------------------------------------------
    # Nodes  (no controller node — affinity shown via switch border color)
    # ------------------------------------------------------------------
    node_list = []

    for sw in switches:
        if isinstance(sw, dict):
            dpid    = sw["dpid"]
            sw_name = sw.get("name", f"S{dpid - 1}")
        else:
            dpid    = sw
            sw_name = f"S{dpid - 1}"

        # Annotate label with utilisation when observations are available
        obs = switch_obs.get(str(dpid), {})
        rho = obs.get("rho", 0)
        extra = f"\nρ={rho*100:.0f}%" if rho > 0 else ""

        ctrl_color = dpid_to_ctrl_color.get(dpid, default_color)
        ctrl_label = next(
            (c.get("label", "Controller") for c in controllers_colored
             if c["_color"] is ctrl_color),
            "Controller",
        )
        node_list.append({
            "id":         str(dpid),
            "label":      sw_name + extra,
            "group":      "switch",
            "title":      f"DPID: {dpid}",
            "color": {
                "background": "#38bdf8",
                "border":     ctrl_color["border"],
                "highlight":  {"background": "#7dd3fc",
                               "border":     ctrl_color["border"]},
            },
            "borderWidth":         3,
            "borderWidthSelected": 4,
            "ctrlLabel":  ctrl_label,
            "ctrlBorder": ctrl_color["border"],
        })

    for h in hosts:
        mac       = h["mac"]
        ip        = h.get("ip")
        name      = h.get("name", "")          # human-readable name set by scenario
        node_type = h.get("node_type", "host")

        # Map node_type → vis group
        group = node_type if node_type in ("iot_sensor", "iot_gateway") else "host"

        # Build label: prefer name > IP > MAC
        if name and ip:
            label = f"{name}\n{ip}"
        elif name:
            label = name
        elif ip:
            label = f"{ip}\n{mac}"
        else:
            label = mac

        title_parts = []
        if name:
            title_parts.append(f"Name: {name}")
        if ip:
            title_parts.append(f"IP: {ip}")
        title_parts += [f"MAC: {mac}", f"Switch: {h['dpid']} Port: {h['port']}"]
        title = "\n".join(title_parts)

        entry: dict = {
            "id":        mac,
            "label":     label,
            "group":     group,
            "dpid":      h["dpid"],
            "port":      h["port"],
            "title":     title,
            "node_type": node_type,
        }
        if name:
            entry["name"] = name
        if ip:
            entry["ip"] = ip
        node_list.append(entry)

    # ------------------------------------------------------------------
    # Edges
    # ------------------------------------------------------------------
    edge_list = []
    seen: set = set()
    for lk in links:
        src  = str(lk["src_dpid"])
        dst  = str(lk["dst_dpid"])
        key  = tuple(sorted((src, dst)))
        if key in seen:
            continue
        seen.add(key)
        cost = lk.get("cost_ms", lk.get("cost", 1))

        # Traffic from port tx rates on both sides of the link
        src_port_stats = stats.get(str(lk["src_dpid"]), {}).get(str(lk["src_port"]), {})
        dst_port_stats = stats.get(str(lk["dst_dpid"]), {}).get(str(lk["dst_port"]), {})
        rate_ab = src_port_stats.get("tx_rate_bps") or 0  # src → dst
        rate_ba = dst_port_stats.get("tx_rate_bps") or 0  # dst → src

        # Fall back to ATVM when port stats are absent
        if rate_ab == 0 and rate_ba == 0:
            rate_ab = atvm_map.get((lk["src_dpid"], lk["dst_dpid"]), 0)
            rate_ba = atvm_map.get((lk["dst_dpid"], lk["src_dpid"]), 0)

        traffic_bps = max(rate_ab, rate_ba)

        # Capacity from port speed; prefer src side, fall back to dst side
        speed_kbps = src_port_stats.get("speed_kbps") or dst_port_stats.get("speed_kbps", 0)
        speed_bps  = speed_kbps * 1000
        util = min(traffic_bps / speed_bps, 1.0) if speed_bps > 0 else 0

        label_cost = f"{cost:.1f}ms" if isinstance(cost, float) else str(cost)
        label_util = f" ({util*100:.0f}%)" if util > 0 else ""

        # Tooltip: bidirectional rates with adaptive units
        tooltip = (f"Link: {lk['src_dpid']}:{lk['src_port']} ↔ "
                   f"{lk['dst_dpid']}:{lk['dst_port']}<br>Cost: {cost}ms")
        if rate_ab > 0 or rate_ba > 0:
            tooltip += (f"<br>→ {_fmt_bps(rate_ab)}  ← {_fmt_bps(rate_ba)}"
                        f"  ({util*100:.1f}% util)")

        edge_list.append({
            "from":  src,
            "to":    dst,
            "label": f"{lk['src_port']}↔{lk['dst_port']} {label_cost}{label_util}",
            "title": tooltip,
            "width": _link_width(util),
            "color": {
                "color":     _utilisation_color(util),
                "highlight": "#7dd3fc",
            },
            "is_host_link": False,
            "src_dpid": lk["src_dpid"],
            "src_port": lk["src_port"],
            "dst_dpid": lk["dst_dpid"],
            "dst_port": lk["dst_port"],
            "cost_ms": cost,
            "rate_ab": rate_ab,
            "rate_ba": rate_ba,
            "speed_bps": speed_bps,
            "util": util,
        })

    # Host → switch edges
    for h in hosts:
        mac   = h["mac"]
        sw    = str(h["dpid"])
        ip    = h.get("ip")
        hname = h.get("name", "")
        ident = hname or ip or mac
        htitle = f"{ident} — switch {h['dpid']} port {h['port']}"
        edge_list.append({
            "from":  mac,
            "to":    sw,
            "label": f"p{h['port']}",
            "title": htitle,
            "dashes": True,
            "color":  {"color": "#64748b", "highlight": "#f472b6"},
            "is_host_link": True,
            "mac": mac,
            "sw": sw,
            "port": h['port'],
        })

    ts = datetime.fromtimestamp(state.get("timestamp", time.time())).strftime("%Y-%m-%d %H:%M:%S")

    html = HTML_TEMPLATE
    html = html.replace("CTRL_LEGEND_PLACEHOLDER",    ctrl_legend_html)
    html = html.replace("NODES_JSON_PLACEHOLDER",     json.dumps(node_list))
    html = html.replace("EDGES_JSON_PLACEHOLDER",     json.dumps(edge_list))
    html = html.replace("STATS_JSON_PLACEHOLDER",     json.dumps(stats))
    html = html.replace("SWITCH_OBS_PLACEHOLDER",     json.dumps(switch_obs))
    html = html.replace("ATVM_JSON_PLACEHOLDER",      json.dumps(atvm))
    html = html.replace("MU_MAX_GLOBAL_PLACEHOLDER",  str(mu_max_global))
    html = html.replace("TIMESTAMP_PLACEHOLDER",      ts)

    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(html)


def main():
    parser = argparse.ArgumentParser(description="SDN topology HTML generator")
    parser.add_argument("--input",  default=_DEFAULT_STATE, help="Path to controller state JSON")
    parser.add_argument("--output", default=_DEFAULT_HTML,  help="Path to write HTML")
    parser.add_argument("--watch",  action="store_true",    help="Regenerate HTML every 2 seconds")
    args = parser.parse_args()

    if args.watch:
        print(f"Watching {args.input} -> {args.output} (Ctrl+C to stop)")
        while True:
            if os.path.exists(args.input):
                try:
                    with open(args.input, "r") as f:
                        state = json.load(f)
                    generate_html(state, args.output)
                    print(f"[{datetime.now().strftime('%H:%M:%S')}] Updated {args.output}")
                except Exception as e:
                    print(f"[{datetime.now().strftime('%H:%M:%S')}] Error: {e}")
            else:
                print(f"[{datetime.now().strftime('%H:%M:%S')}] Waiting for {args.input}...")
            time.sleep(2)
    else:
        if not os.path.exists(args.input):
            raise FileNotFoundError(f"State file not found: {args.input}")
        with open(args.input, "r") as f:
            state = json.load(f)
        generate_html(state, args.output)
        print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
