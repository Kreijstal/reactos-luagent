register_op("port_close", function(req)
  local relay_id = tonumber(req.relay_id or "")
  if not relay_id then
    error("relay_id is required")
  end

  agent.port_close(relay_id)
  return {
    status = "ok",
    relay_id = relay_id
  }
end)
