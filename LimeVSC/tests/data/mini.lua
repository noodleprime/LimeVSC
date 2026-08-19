---@meta
-- Synthetic LuaLS stub covering every shape LuaLSProvider must handle.
-- Deliberately mirrors the real Lime.lua's conventions, including the two
-- traps: a constructor whose parameters exist only on @overload lines, and a
-- @field whose type is an event class.

Demo = {}

--- @class Demo
--- @field count number
--- @field onFired Demo_onFired
Demo = {}

--- Returns the current count.
--- @return number
function Demo.getCount() end

--- Sets the display name.
--- @param name string
--- @param prefix string?
function Demo.setName(name, prefix) end

--- @class Demo_onFired
Demo_onFired = {}
--- @param Function fun(amount: number)
--- @return Hook
function Demo_onFired:hook(Function) end
function Demo_onFired:clear() end

--- @class Widget
--- @operator add(Widget): Widget
--- @field width number
--- @field height number
--- @field onResized Widget_onResized
Widget = {}

--- Declared BEFORE Widget.new on purpose: whether this event is per-instance
--- depends on Widget being constructible, which is not known until later in
--- the file. Exercises the second reconciliation pass.
--- @class Widget_onResized
Widget_onResized = {}
--- @param Function fun(w: number, h: number)
--- @return Hook
function Widget_onResized:hook(Function) end

--- @overload fun(w: number, h: number): Widget
--- @return Widget
function Widget.new() end

--- Resizes the widget.
--- @param size number
function Widget:resize(size) end
