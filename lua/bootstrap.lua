local ops = {}

local function parse_kv(payload)
  local out = {}
  for line in string.gmatch(payload or "", "[^\r\n]+") do
    local key, value = string.match(line, "^([^=]+)=(.*)$")
    if key then
      out[key] = value
    end
  end
  return out
end

local function encode_result(tbl)
  local lines = {}
  for k, v in pairs(tbl) do
    lines[#lines + 1] = tostring(k) .. "=" .. tostring(v)
  end
  table.sort(lines)
  return table.concat(lines, "\n")
end

function register_op(name, fn)
  ops[name] = fn
end

function dispatch_request(req_id, payload)
  local req = parse_kv(payload)
  local name = req.name
  local handler = name and ops[name] or nil

  if not handler then
    agent.error(req_id, "unknown_op", "no handler registered")
    return
  end

  local ok, result = pcall(handler, req)
  if not ok then
    agent.error(req_id, "internal", result)
    return
  end

  agent.reply(req_id, "OP_RESULT", encode_result(result or { status = "ok" }))
end

dofile("lua/ops/list.lua")
dofile("lua/ops/debug_sessions.lua")
dofile("lua/ops/debug_transfers.lua")
dofile("lua/ops/debug_procs.lua")
dofile("lua/ops/debug_stats.lua")
dofile("lua/ops/port_open.lua")
dofile("lua/ops/port_close.lua")
dofile("lua/ops/port_list.lua")
dofile("lua/ops/tool_run.lua")
