function Component()
{
    installer.installationStarted.connect(this, Component.prototype.onInstallationStarted);
}

Component.prototype.onInstallationStarted = function()
{
    if (component.updateRequested() || component.installationRequested()) {
        if (installer.value("os") == "win")
            component.installerbaseBinaryPath = "@TargetDir@/tempMaintenanceTool.exe";
        else if (installer.value("os") == "x11")
            component.installerbaseBinaryPath = "@TargetDir@/.tempMaintenanceTool";
        else if (installer.value("os") == "mac")
            component.installerbaseBinaryPath = "@TargetDir@/MaintenanceTool.app";
        installer.setInstallerBaseBinary(component.installerbaseBinaryPath);

        // Update resource file (branding) used when QtIFW updates the MaintenanceTool base binary.
        var updateResourceFilePath = installer.value("TargetDir") + "/update.rcc";
        installer.setValue("DefaultResourceReplacement", updateResourceFilePath);
    }
}

Component.prototype.createOperations = function()
{
    // Call the base createOperations and afterwards set some registry settings (unpacking ...)
    component.createOperations();

    var editionName = "Atlas";

    // Create uninstall link only for windows
    if (installer.value("os") == "win")
    {
        // shortcut to uninstaller
        component.addOperation( "CreateShortcut",
                                "@TargetDir@//@MaintenanceToolName@.exe",
                                "@StartMenuDir@/Uninstall " + editionName + ".lnk",
                                " --uninstall");
    }
    // only for windows online installer
    if ( installer.value("os") == "win" && !installer.isOfflineOnly() )
    {
        // create shortcut
        component.addOperation( "CreateShortcut",
                                "@TargetDir@//@MaintenanceToolName@.exe",
                                "@StartMenuDir@/" + editionName + " Maintenance Tool.lnk",
                                "workingDirectory=@TargetDir@" );
        component.addOperation("CreateShortcut", "@TargetDir@/@MaintenanceToolName@.exe", "@StartMenuDir@/Manage Packages.lnk", "--manage-packages");
        component.addOperation("CreateShortcut", "@TargetDir@/@MaintenanceToolName@.exe", "@StartMenuDir@/Update " + editionName + ".lnk", "--updater");
    }
    if ( installer.value("os") == "x11" )
    {
        // only for online installer
        if (!installer.isOfflineOnly()) {
            component.addOperation( "CreateDesktopEntry",
                                    editionName + "-MaintenanceTool.desktop",
                                    "Encoding=UTF-8\nVersion=1.1\nType=Application\nExec=@TargetDir@/MaintenanceTool\nPath=@TargetDir@\nName=" + editionName + " Maintenance Tool\nGenericName=Install or uninstall Atlas components.\nIcon=@TargetDir@/Atlas.AppDir/Atlas.png\nTerminal=false\nComment=Image Analysis\nCategories=Graphics;Science;Utility;\nStartupWMClass=AtlasMaintenanceTool"
                                   );
        }
    }
}
