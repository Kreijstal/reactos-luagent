local function collect_argv(req, path)
  local argv = {}
  local i = 0
  while true do
    local key = "argv" .. tostring(i)
    local value = req[key]
    if value == nil then
      break
    end
    argv[#argv + 1] = value
    i = i + 1
  end
  if #argv == 0 then
    argv[1] = path
  end
  return argv
end

register_op("tool.run", function(req)
  local path = req.path
  if not path then
    error("path is required")
  end

  local timeout_ms = tonumber(req.timeout_ms or "60000") or 60000
  local idle_timeout_ms = tonumber(req.idle_timeout_ms or "10000") or 10000
  local proc = agent.proc_spawn(path, collect_argv(req, path), timeout_ms, idle_timeout_ms)

  return {
    status = "ok",
    proc_id = proc.proc_id,
    path = proc.path
  }
end)
