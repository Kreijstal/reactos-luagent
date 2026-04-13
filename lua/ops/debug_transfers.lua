register_op("debug.transfers", function(req)
  local out = agent.debug_transfers()
  out.status = "ok"
  return out
end)
