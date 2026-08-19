Lime.onInit:hook(function()
    Lime.setInitConfig(Lime.Enum.DriverType.Direct3D9, Vec2.new(640, 480))
    Lime.setDebugConfig(true, true)
end)

Lime.onStart:hook(function()
    Lime.log("Hello, World!")
end)

Lime.onUpdate:hook(function(dt)
    
end)

Lime.onClose:hook(function()

end)