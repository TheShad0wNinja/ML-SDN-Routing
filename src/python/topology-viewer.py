#!/usr/bin/env python3
"""
SDN Topology Visualizer
Reads controller state JSON and writes an interactive, self-contained HTML file.
Usage:
    python visualizer.py --watch
"""
import argparse
import json
import os
import time
from datetime import datetime

_SCRATCH_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_DEFAULT_STATE = os.path.join(_SCRATCH_ROOT, "data", "state", "sdn_state.json")
_DEFAULT_HTML = os.path.join(_SCRATCH_ROOT, "data", "html", "topology.html")

# ---------------------------------------------------------------------------
# HTML template using vis-network (cdnjs - stable CDN)
# ---------------------------------------------------------------------------
HTML_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>SDN Topology</title>
<script src="https://cdnjs.cloudflare.com/ajax/libs/vis/4.21.0/vis.min.js"></script>
<link href="https://cdnjs.cloudflare.com/ajax/libs/vis/4.21.0/vis.min.css" rel="stylesheet" />
<style>
body {
  margin: 0;
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  background: #0f172a;
  color: #e2e8f0;
}
#network {
  width: 100vw;
  height: 100vh;
}
#panel {
  position: absolute;
  top: 12px;
  right: 12px;
  width: 340px;
  background: rgba(15, 23, 42, 0.95);
  border: 1px solid #334155;
  padding: 16px;
  border-radius: 10px;
  max-height: 90vh;
  overflow-y: auto;
  font-size: 13px;
  box-shadow: 0 10px 30px rgba(0,0,0,0.5);
  z-index: 10;
}
h3 {
  margin-top: 0;
  color: #38bdf8;
}
h4 {
  color: #94a3b8;
  margin-bottom: 6px;
}
p {
  margin: 4px 0;
}
table {
  width: 100%;
  border-collapse: collapse;
  margin-top: 8px;
  font-size: 12px;
}
th, td {
  text-align: left;
  padding: 5px;
  border-bottom: 1px solid #334155;
}
th {
  color: #94a3b8;
  font-weight: 600;
}
.legend {
  position: absolute;
  bottom: 12px;
  left: 12px;
  background: rgba(15, 23, 42, 0.95);
  border: 1px solid #334155;
  padding: 10px 14px;
  border-radius: 8px;
  font-size: 12px;
  z-index: 10;
}
.legend-item {
  display: flex;
  align-items: center;
  margin: 5px 0;
}
.legend-box {
  width: 14px;
  height: 14px;
  margin-right: 8px;
  border-radius: 3px;
}
.legend-dot {
  width: 12px;
  height: 12px;
  border-radius: 50%;
  margin-right: 9px;
  margin-left: 1px;
}
.timestamp {
  position: absolute;
  top: 12px;
  left: 12px;
  background: rgba(15, 23, 42, 0.95);
  border: 1px solid #334155;
  padding: 8px 12px;
  border-radius: 8px;
  font-size: 12px;
  color: #94a3b8;
  z-index: 10;
}
</style>
</head>
<body>
<div id="network"></div>
<div class="timestamp">Last updated: TIMESTAMP_PLACEHOLDER</div>
<div class="legend">
  <div class="legend-item"><div class="legend-box" style="background:#fbbf24;transform:rotate(45deg);width:12px;height:12px;border-radius:2px"></div>Controller</div>
  <div class="legend-item"><div class="legend-box" style="background:#38bdf8"></div>Switch</div>
  <div class="legend-item"><div class="legend-dot" style="background:#f472b6"></div>Host</div>
</div>
<div id="panel">
  <h3>Network Overview</h3>
  <p><b>Controller:</b> <span id="ctrl-count">0</span></p>
  <p><b>Switches:</b> <span id="sw-count">0</span></p>
  <p><b>Hosts:</b> <span id="host-count">0</span></p>
  <p><b>Links:</b> <span id="link-count">0</span></p>
  <hr style="border-color:#334155;margin:12px 0;">
  <p style="color:#64748b;">Click the controller, a switch, or a host for details.</p>
</div>

<script>
const nodeData = NODES_JSON_PLACEHOLDER;
const edgeData = EDGES_JSON_PLACEHOLDER;
const statsData = STATS_JSON_PLACEHOLDER;

const nodes = new vis.DataSet(nodeData);
const edges = new vis.DataSet(edgeData);

const container = document.getElementById('network');
const data = { nodes: nodes, edges: edges };

const options = {
  groups: {
    switch: {
      shape: 'box',
      color: {
        background: '#38bdf8',
        border: '#0ea5e9',
        highlight: { background: '#7dd3fc', border: '#38bdf8' }
      },
      font: { color: '#0f172a', face: 'sans-serif', size: 14 },
      margin: 10,
      shadow: { enabled: true, color: 'rgba(0,0,0,0.5)', size: 10 }
    },
    host: {
      shape: 'dot',
      color: {
        background: '#f472b6',
        border: '#db2777',
        highlight: { background: '#f9a8d4', border: '#f472b6' }
      },
      font: {
        color: '#f8fafc',
        face: 'sans-serif',
        size: 17,
        bold: true,
        multi: true,
        vadjust: 0
      },
      size: 24,
      shadow: { enabled: true, color: 'rgba(0,0,0,0.5)', size: 12 }
    },
    controller: {
      shape: 'diamond',
      size: 30,
      color: {
        background: '#fbbf24',
        border: '#d97706',
        highlight: { background: '#fde047', border: '#fbbf24' }
      },
      font: { color: '#0f172a', face: 'sans-serif', size: 14, bold: true },
      margin: 12,
      shadow: { enabled: true, color: 'rgba(0,0,0,0.5)', size: 10 }
    }
  },
  edges: {
    width: 2,
    color: { color: '#475569', highlight: '#38bdf8', hover: '#94a3b8' },
    smooth: { type: 'continuous' },
    font: { color: '#111', size: 14, align: 'middle', background: { enabled: true, color: '#00f' } }
  },
  physics: {
    stabilization: { iterations: 200 },
    barnesHut: {
      gravitationalConstant: -4000,
      centralGravity: 0.3,
      springLength: 140,
      springConstant: 0.04
    }
  },
  interaction: { hover: true, tooltipDelay: 150 }
};

const network = new vis.Network(container, data, options);

network.once('stabilizationIterationsDone', function () {
  const hostIds = nodes.get({ filter: function (n) { return n.group === 'host'; } }).map(function (n) { return n.id; });
  const focus = new Set();
  if (hostIds.length > 0) {
    hostIds.forEach(function (hid) { focus.add(hid); });
    hostIds.forEach(function (hid) {
      network.getConnectedNodes(hid).forEach(function (cid) { focus.add(cid); });
    });
  }
  nodes.get({ filter: function (n) { return n.group === 'controller'; } }).forEach(function (n) {
    focus.add(n.id);
    network.getConnectedNodes(n.id).forEach(function (cid) { focus.add(cid); });
  });
  if (focus.size > 0) {
    network.fit({
      nodes: Array.from(focus),
      animation: { duration: 400, easingFunction: 'easeInOutQuad' },
      padding: 95
    });
  }
});

document.getElementById('ctrl-count').textContent = nodes.get({ filter: n => n.group === 'controller' }).length;
document.getElementById('sw-count').textContent = nodes.get({ filter: n => n.group === 'switch' }).length;
document.getElementById('host-count').textContent = nodes.get({ filter: n => n.group === 'host' }).length;
document.getElementById('link-count').textContent = edges.length;

network.on("click", function(params) {
  const panel = document.getElementById('panel');
  if (params.nodes.length === 0) {
    panel.innerHTML = `
      <h3>Network Overview</h3>
      <p><b>Controller:</b> <span id="ctrl-count">${nodes.get({ filter: n => n.group === 'controller' }).length}</span></p>
      <p><b>Switches:</b> <span id="sw-count">${nodes.get({ filter: n => n.group === 'switch' }).length}</span></p>
      <p><b>Hosts:</b> <span id="host-count">${nodes.get({ filter: n => n.group === 'host' }).length}</span></p>
      <p><b>Links:</b> <span id="link-count">${edges.length}</span></p>
      <hr style="border-color:#334155;margin:12px 0;">
      <p style="color:#64748b;">Click the controller, a switch, or a host for details.</p>
    `;
    return;
  }

  const nodeId = params.nodes[0];
  const node = nodes.get(nodeId);
  let html = '<h3>' + (node.label || nodeId) + '</h3>';
  html += '<p><b>Type:</b> <span style="text-transform:capitalize">' + node.group + '</span></p>';

  if (node.group === 'controller') {
    html += '<p><b>Role:</b> Control plane (OpenFlow 1.3)</p>';
    if (node.title) {
      html += '<p style="margin-top:10px;line-height:1.45">' + node.title.replace(/\\n/g, '<br>') + '</p>';
    }
    html += '<p style="color:#64748b;margin-top:12px;">Yellow dashed edges are logical OpenFlow sessions to each switch (includes the in-simulation channel and the ZMQ bridge to this app).</p>';
  } else if (node.group === 'switch') {
    html += '<p><b>DPID:</b> ' + nodeId + '</p>';
    const swStats = statsData[String(nodeId)];
    if (swStats && Object.keys(swStats).length > 0) {
      html += '<h4>Port Statistics</h4>';
      html += '<table><tr><th>Port</th><th>RX Pkts</th><th>TX Pkts</th><th>RX Bytes</th><th>TX Bytes</th></tr>';
      for (const [portNo, s] of Object.entries(swStats)) {
        html += '<tr>';
        html += '<td>' + portNo + '</td>';
        html += '<td>' + (s.rx_packets ?? 0).toLocaleString() + '</td>';
        html += '<td>' + (s.tx_packets ?? 0).toLocaleString() + '</td>';
        html += '<td>' + (s.rx_bytes ?? 0).toLocaleString() + '</td>';
        html += '<td>' + (s.tx_bytes ?? 0).toLocaleString() + '</td>';
        html += '</tr>';
      }
      html += '</table>';
    } else {
      html += '<p style="color:#64748b;">No port stats received yet.</p>';
    }
  } else if (node.group === 'host') {
    html += '<p><b>IP:</b> ' + (node.ip ? '<span style="color:#7dd3fc;font-size:1.1em">' + node.ip + '</span>' : '<span style="color:#64748b">unknown (no IPv4 in state yet)</span>') + '</p>';
    html += '<p><b>MAC:</b> ' + nodeId + '</p>';
    html += '<p><b>Connected to switch:</b> ' + (node.dpid || 'unknown') + ' <b>on port:</b> ' + (node.port || 'unknown') + '</p>';
  }
  panel.innerHTML = html;
});
</script>
</body>
</html>
"""


def generate_html(state, output_path):
    switches = state.get("switches", [])
    hosts = state.get("hosts", [])
    links = state.get("links", [])
    stats = state.get("stats", {})
    ctrl_meta = dict(state.get("controller") or {})
    control_links = state.get("control_links")
    if control_links is None:
        control_links = [{"dpid": d} for d in switches]

    # Build vis-network nodes
    node_list = []
    ctrl_id = ctrl_meta.get("id") or "sdn-controller"
    if switches or control_links:
        node_list.append({
            "id": ctrl_id,
            "label": ctrl_meta.get("label", "SDN Controller"),
            "group": "controller",
            "title": ctrl_meta.get(
                "detail",
                "OpenFlow control channel to switches (in-simulation + ZMQ to this app).",
            ),
        })

    for sw in switches:
        node_list.append({
            "id": str(sw),
            "label": f"Switch {sw}",
            "group": "switch",
            "title": f"DPID: {sw}"
        })
    
    for h in hosts:
        mac = h["mac"]
        ip = h.get("ip")
        if ip:
            label = f"{ip}\n{mac}"
            title = f"IP: {ip}\nMAC: {mac}\nSwitch: {h['dpid']} Port: {h['port']}"
        else:
            label = mac
            title = f"MAC: {mac}\nSwitch: {h['dpid']} Port: {h['port']}\n(IP appears after controller sees IPv4 traffic)"
        entry = {
            "id": mac,
            "label": label,
            "group": "host",
            "dpid": h["dpid"],
            "port": h["port"],
            "title": title,
        }
        if ip:
            entry["ip"] = ip
        node_list.append(entry)

    # Build edges (deduplicated)
    edge_list = []
    if switches or control_links:
        for cl in control_links:
            dpid = cl.get("dpid")
            if dpid is None:
                continue
            edge_list.append({
                "from": ctrl_id,
                "to": str(dpid),
                "label": "OpenFlow",
                "title": f"Control plane: {ctrl_id} ↔ switch {dpid}",
                "dashes": [8, 4],
                "color": {"color": "#f59e0b", "highlight": "#fde047"},
                "width": 2,
            })

    seen = set()
    for lk in links:
        src = str(lk["src_dpid"])
        dst = str(lk["dst_dpid"])
        key = tuple(sorted((src, dst)))
        if key in seen:
            continue
        seen.add(key)
        edge_list.append({
            "from": src,
            "to": dst,
            "label": f"{lk['src_port']}↔{lk['dst_port']}",
            "title": f"Link: {lk['src_dpid']}:{lk['src_port']} ↔ {lk['dst_dpid']}:{lk['dst_port']}<br>Cost: {lk.get('cost', 1)}"
        })

    # Attach hosts to their access switch (LLDP only discovers switch–switch links).
    for h in hosts:
        mac = h["mac"]
        sw = str(h["dpid"])
        ip = h.get("ip")
        htitle = f"Host {ip + ' — ' if ip else ''}{mac} — switch {h['dpid']} port {h['port']}"
        edge_list.append({
            "from": mac,
            "to": sw,
            "label": f"p{h['port']}",
            "title": htitle,
            "dashes": True,
            "color": {"color": "#64748b", "highlight": "#f472b6"}
        })

    ts = datetime.fromtimestamp(state.get("timestamp", time.time())).strftime("%Y-%m-%d %H:%M:%S")

    html = HTML_TEMPLATE
    html = html.replace("NODES_JSON_PLACEHOLDER", json.dumps(node_list))
    html = html.replace("EDGES_JSON_PLACEHOLDER", json.dumps(edge_list))
    html = html.replace("STATS_JSON_PLACEHOLDER", json.dumps(stats))
    html = html.replace("TIMESTAMP_PLACEHOLDER", ts)

    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(html)


def main():
    parser = argparse.ArgumentParser(description="SDN topology HTML generator")
    parser.add_argument("--input", default=_DEFAULT_STATE, help="Path to controller state JSON")
    parser.add_argument("--output", default=_DEFAULT_HTML, help="Path to write HTML")
    parser.add_argument("--watch", action="store_true", help="Regenerate HTML every 2 seconds")
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