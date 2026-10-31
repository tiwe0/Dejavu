-- Resolve the Manifest launcher activity namespace through Java reflection.

local manifest = {}

function manifest.main_activity()
    local app = java.invoke('android.app.ActivityThread', 'currentApplication',
        '()Landroid/app/Application;')
    local package = java.invoke(app, 'getPackageName', '()Ljava/lang/String;')
    local manager = java.invoke(app, 'getPackageManager',
        '()Landroid/content/pm/PackageManager;')
    local intent = java.invoke(manager, 'getLaunchIntentForPackage',
        '(Ljava/lang/String;)Landroid/content/Intent;', package)
    local class_name = nil
    if intent ~= nil then
        local component = java.invoke(intent, 'getComponent',
            '()Landroid/content/ComponentName;')
        if component ~= nil then
            class_name = java.invoke(component, 'getClassName', '()Ljava/lang/String;')
            java.release(component)
        end
        java.release(intent)
    end
    java.release(manager)
    java.release(app)
    if class_name and class_name:sub(1, 1) == '.' then
        class_name = package .. class_name
    end
    local namespace = class_name and class_name:match('^(.*)%.') or package
    return namespace, package, class_name
end

return manifest
