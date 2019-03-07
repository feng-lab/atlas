function Component()
{
}

Component.prototype.isDefault = function()
{
    // select the component by default
    return true;
}

Component.prototype.createOperations = function()
{
    try {
        // call the base create operations function
        component.createOperations();

        if (installer.value("os") == "win") {
            component.addOperation("CreateShortcut", "@TargetDir@/Atlas/Atlas.exe", "@StartMenuDir@/Atlas.lnk");
        }
        if (installer.value("os") == "x11") {
            component.addOperation("CreateDesktopEntry", "Atlas.desktop",
                               "Encoding=UTF-8\nVersion=1.1\nType=Application\nTerminal=false\nExec=@TargetDir@/Atlas.AppDir/Atlas %F\nName=@Name@\nIcon=@TargetDir@/Atlas.AppDir/Atlas.png\nComment=Image Analysis\nCategories=Graphics;Science;Utility;\nStartupWMClass=@Name@");
        }
    } catch (e) {
        console.log(e);
    }
    
}
