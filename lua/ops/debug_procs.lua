register_op("debug.procs", function(req)
  local out = agent.debug_procs()
  out.status = "ok"
  return out
end)
