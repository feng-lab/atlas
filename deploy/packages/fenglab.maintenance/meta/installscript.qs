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

        // update resource file
        var updateResourceFilePath = "@TargetDir@/update.rcc";
        var normalizedUpdateResourceFilePath = updateResourceFilePath.replace(/@TargetDir@/, installer.value("TargetDir"));
        print("Updating resource file: " + normalizedUpdateResourceFilePath);
        installer.setValue("DefaultResourceReplacement", normalizedUpdateResourceFilePath);
    }
}

Component.prototype.createOperationsForArchive = function(archive)
{
    //installer.performOperation in older versions of the installer framework don't supports @TargetDir@
    var normalizedInstallerbaseBinaryPath = component.installerbaseBinaryPath.replace(/@TargetDir@/,
        installer.value("TargetDir"));
    var normalizedInstallerbaseBinaryBackupPath = normalizedInstallerbaseBinaryPath + "_backup";

    if (installer.value("os") == "mac") {
        // When updating the maintenance tool, move the existing bundle out of the way before extracting the new one.
        // QtIFW leaves this backup in place unless we clean it up.
        if (installer.fileExists(normalizedInstallerbaseBinaryPath)) {
            installer.performOperation("Move",
                new Array(normalizedInstallerbaseBinaryPath, normalizedInstallerbaseBinaryBackupPath));
        }
    } else {
        if (installer.fileExists(normalizedInstallerbaseBinaryPath)) {
            installer.performOperation("SimpleMoveFile",
                new Array(normalizedInstallerbaseBinaryPath, normalizedInstallerbaseBinaryBackupPath));
        }
    }
    component.createOperationsForArchive(archive);

    // On macOS the backup is an app bundle directory (MaintenanceTool.app_backup). When running the original
    // installer (as opposed to running from the MaintenanceTool itself), it's safe to remove the backup after we
    // have extracted the new bundle; leaving it around confuses users and grows the install dir over time.
    if (installer.value("os") == "mac" && installer.isInstaller()) {
        // Queue cleanup *after* Extract by using an operation (rather than performing immediately).
        // rm -rf with -f makes the cleanup idempotent; allow exit code 1 so a non-critical cleanup failure does not
        // fail the whole installation.
        component.addOperation("Execute", "{0,1}", "/bin/rm", "-rf", normalizedInstallerbaseBinaryBackupPath);
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
