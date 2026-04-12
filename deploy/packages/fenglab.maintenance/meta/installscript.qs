function Component()
{
    component.ifwVersion = installer.value("FrameworkVersion");
    installer.installationStarted.connect(this, Component.prototype.onInstallationStarted);
}

Component.prototype.onInstallationStarted = function()
{
    if (component.updateRequested() || component.installationRequested()) {
        var updateResourceFilePath = installer.value("TargetDir");
        if (installer.value("os") == "win")
            component.installerbaseBinaryPath = "@TargetDir@/tempMaintenanceTool.exe";
        else if (installer.value("os") == "x11")
            component.installerbaseBinaryPath = "@TargetDir@/.tempMaintenanceTool";
        else if (installer.value("os") == "mac") {
            // In macOs maintenance tool can be either installerbase from Qt Installer
            // Framework's install folder, or app bundle created by binarycreator
            // with --create-maintenancetool switch. "MaintenanceTool.app" -name
            // may differ depending on what has been defined in config.xml while
            // creating the maintenance tool.
            // Use either of the following (not both):

            // component.installerbaseBinaryPath = "@TargetDir@/installerbase";
            if (installer.versionMatches(component.ifwVersion, "<4.8.0")) {
                component.installerbaseBinaryPath = "@TargetDir@/MaintenanceTool.app";
            } else {
                updateResourceFilePath += "/tmpMaintenanceToolApp";
                component.installerbaseBinaryPath = "@TargetDir@/tmpMaintenanceToolApp/MaintenanceTool.app";
            }
        }
        installer.setInstallerBaseBinary(component.installerbaseBinaryPath);

        // Update resource file (branding) used when QtIFW updates the MaintenanceTool base binary.
        updateResourceFilePath += "/update.rcc";
        installer.setValue("DefaultResourceReplacement", updateResourceFilePath);
    }
}

Component.prototype.createOperationsForArchive = function(archive)
{
    // IFW versions 4.8.1 onwards supports extracting the maintenance tool to a folder.
    // It is a good practice to extract the maintenance tool to a folder in macOs, so
    // it won't interfere the current running maintenance tool. As the last step of the
    // installation, IFW will move the maintenance tool to the root of the installation.
    // Windows is using deferred update for the maintenance tool, and Linux inode.
    if (installer.versionMatches(component.ifwVersion, "<4.8.0") || (installer.value("os") != "mac"))
        component.createOperationsForArchive(archive);
    else
        component.addOperation("Extract", archive, "@TargetDir@/tmpMaintenanceToolApp");
}

Component.prototype.createOperations = function()
{
    // Call the base createOperations and afterwards set some registry settings (unpacking ...)
    component.createOperations();

    if (component.updateRequested() || component.installationRequested()) {
        var maintenanceIniPath = "@TargetDir@/@MaintenanceToolName@.ini";

        // Force the updated maintenance tool to rebuild its default repository list
        // from the embedded RemoteRepositories config instead of stale persisted state.
        component.addOperation(
            "Settings",
            "path=" + maintenanceIniPath,
            "method=remove",
            "key=DefaultRepositories",
            "value=",
            "UNDOOPERATION", ""
        );
    }

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
