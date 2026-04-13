register_op("port_list", function(req)
  local relays = agent.port_list()
  return {
    status = "ok",
    count = #relays
  }
end)
