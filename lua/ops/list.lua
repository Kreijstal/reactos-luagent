register_op("list", function(req)
  local path = req.path or "."
  local entries = agent.fs_list(path)

  return {
    status = "ok",
    path = path,
    count = #entries
  }
end)
