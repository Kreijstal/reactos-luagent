register_op("debug.stats", function(req)
  local out = agent.debug_stats()
  out.status = "ok"
  return out
end)
