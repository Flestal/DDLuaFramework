dd.log("base mods lua folder discovered")
assert(type(dd.effect) == "function", "dd.effect API missing")
dd.effect("ddlua_test_effect", {
    before_apply = function(_context)
        dd.log("test effect before_apply")
        return true
    end,
    after_apply = function(_context)
        dd.log("test effect after_apply")
    end,
})
dd.on("corridor_return_roll", function(context)
    local has_marker = false
    if dd.game_build ~= "" then
        for _, actor in ipairs(context:actors("heroes")) do
            if actor:has_buff_id("missing_test_marker") then
                has_marker = true
                break
            end
        end
    end
    dd.log("test corridor callback", context.content,
        context.native_chance, string.format("%.1f", context.torchlight),
        has_marker)
    return { chance_multiplier_delta = 0.5, extra_rolls = 1 }
end)
