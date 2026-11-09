
-- TikTok region spoof - Lua package entry (start/stop/status contract).
--
-- com.zhiliaoapp.musically derives its store region from SIM/carrier/device
-- signals sent to /service/2/device_register/. When those say CN, the server
-- returns a server.json whose TTNet rule 3003000 "cn_block" drops
-- /aweme/v2/feed/ locally (net::ERR_TTNET_TRAFFIC_CONTROL_DROP, -555), so the
-- feed shows an error.
--
-- This plugin hooks the TikTok device-info helper X.05DL and the framework
-- telephony getters so device_register/region params report US/T-Mobile before
-- TikTok fetches its region config. The first hook targets X.05DL, a class that
-- only exists in TikTok; in any other process class resolution fails and no
-- hooks are installed.

local HOOKS = {
    -- TikTok device-info helper used by the device_register request builder.
    { "X.05DL", "LIZ", "()Ljava/lang/String;", "US", "05DL.LIZ" },
    { "X.05DL", "LIZJ", "()Ljava/lang/String;", "310260", "05DL.LIZJ" },
    { "X.05DL", "LIZLLL", "()Ljava/lang/String;", "T-Mobile", "05DL.LIZLLL" },
    { "X.05DL", "LJ", "()Ljava/lang/String;", "US", "05DL.LJ" },
    -- Framework fallbacks (BPEA-protected callers still reach these methods).
    { "android.telephony.TelephonyManager", "getSimCountryIso", "()Ljava/lang/String;", "us", "TM.getSimCountryIso" },
    { "android.telephony.TelephonyManager", "getNetworkCountryIso", "()Ljava/lang/String;", "us", "TM.getNetworkCountryIso" },
    { "android.telephony.TelephonyManager", "getSimOperator", "()Ljava/lang/String;", "310260", "TM.getSimOperator" },
    { "android.telephony.TelephonyManager", "getNetworkOperator", "()Ljava/lang/String;", "310260", "TM.getNetworkOperator" },
    { "android.telephony.TelephonyManager", "getSimOperatorName", "()Ljava/lang/String;", "T-Mobile", "TM.getSimOperatorName" },
    { "android.telephony.TelephonyManager", "getNetworkOperatorName", "()Ljava/lang/String;", "T-Mobile", "TM.getNetworkOperatorName" },
}

local state = nil

local function source_for(value, label)
    return string.format([[
int before_hook(dejavu_hook_context *context) {
    dejavu_hook_log("tiktok-region hit %s");
    return dejavu_hook_set_result_string_mutf8(context, "%s", %d);
}
]], label, value, #value)
end

local function install_all()
    if state ~= nil then
        return true, "already installed"
    end
    if type(hook) ~= "table" or type(hook.install) ~= "function" then
        return false, "hook.install is unavailable in this Lua session"
    end
    -- Guard: X.05DL exists only in TikTok. If this fails we are in another
    -- target process and must not touch any framework method.
    local guard_ok, guard_id = pcall(hook.install, HOOKS[1][1], HOOKS[1][2], HOOKS[1][3],
        source_for(HOOKS[1][4], HOOKS[1][5]))
    if not guard_ok then
        return false, "X.05DL not found; target is not com.zhiliaoapp.musically: " ..
            tostring(guard_id)
    end
    local ids = { guard_id }
    local failures = {}
    for index = 2, #HOOKS do
        local item = HOOKS[index]
        local ok, id = pcall(hook.install, item[1], item[2], item[3],
            source_for(item[4], item[5]))
        if ok then
            ids[#ids + 1] = id
        else
            failures[#failures + 1] = item[5] .. ": " .. tostring(id)
        end
    end
    state = { ids = ids, failures = failures }
    local message = string.format("installed %d/%d hooks", #ids, #HOOKS)
    if #failures > 0 then
        message = message .. "; failures: " .. table.concat(failures, "; ")
    end
    return true, message
end

local function uninstall_all()
    if state == nil then
        return false, "not installed"
    end
    local removed = 0
    local failures = {}
    for _, id in ipairs(state.ids) do
        local ok, result = pcall(hook.uninstall, id)
        if ok then
            removed = removed + 1
        else
            failures[#failures + 1] = tostring(result)
        end
    end
    local total = #state.ids
    state = nil
    local message = string.format("uninstalled %d/%d hooks", removed, total)
    if #failures > 0 then
        return false, message .. "; failures: " .. table.concat(failures, "; ")
    end
    return true, message
end

local function log(message)
    pcall(hook.log, "tiktok-region: " .. message)
end

return {
    start = function(ctx)
        local ok, message = install_all()
        if not ok then
            error(message, 0)
        end
        log(message)
        return true
    end,
    stop = function(ctx)
        local ok, message = uninstall_all()
        if not ok then
            if message == "not installed" then
                return true
            end
            error(message, 0)
        end
        log(message)
        return true
    end,
    status = function(ctx)
        if state == nil then
            return "stopped"
        end
        local message = string.format("running (%d hooks", #state.ids)
        if #state.failures > 0 then
            message = message .. ", " .. #state.failures .. " failed"
        end
        return message .. ")"
    end,
}
