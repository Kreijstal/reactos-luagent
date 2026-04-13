register_op("port_open", function(req)
  local listen_host = req.listen_host or "0.0.0.0"
  local listen_port = tonumber(req.listen_port or "0") or 0
  local target_host = req.target_host or "127.0.0.1"
  local target_port = tonumber(req.target_port or "")
  if not target_port then
    error("target_port is required")
  end

  local relay = agent.port_open(listen_host, listen_port, target_host, target_port)
  return {
    status = "ok",
    relay_id = relay.relay_id,
    listen_port = relay.listen_port,
    target_host = target_host,
    target_port = target_port
  }
end)
