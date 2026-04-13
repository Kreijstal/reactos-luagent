register_op("debug.sessions", function(req)
  local out = agent.debug_sessions()
  out.status = "ok"
  return out
end)
