function Controller()
{
    var restartPage = gui.pageByObjectName("RestartPage");
    if (restartPage) {
        restartPage.entered.connect(this, Controller.prototype.onRestartPageEntered);
    }

    var finishedPage = gui.pageByObjectName("FinishedPage");
    if (finishedPage) {
        finishedPage.entered.connect(this, Controller.prototype.onFinishedPageEntered);
    }
}

Controller.prototype._setLabelText = function(page, labelName, text)
{
    if (page && page[labelName]) {
        page[labelName].setText(text);
    }
}

Controller.prototype._setButtonText = function(buttonId, text)
{
    var button = gui.button(buttonId);
    if (button) {
        button.setText(text);
    }
}

Controller.prototype.onRestartPageEntered = function()
{
    var page = gui.pageByObjectName("RestartPage");
    if (!page) {
        return;
    }

    var restartMessage =
        "<html><body>"
        + "<p><b>Atlas updater was updated successfully.</b></p>"
        + "<p>Atlas packages have not been updated yet.</p>"
        + "<p>Select <b>Restart</b> now. After the updater restarts, run the update again to install the Atlas package updates.</p>"
        + "</body></html>";

    Controller.prototype._setLabelText(page, "MessageLabel", restartMessage);
    Controller.prototype._setLabelText(
        page,
        "InformationLabel",
        "Restart the updater now, then run it again to finish updating Atlas."
    );

    Controller.prototype._setButtonText(buttons.CommitButton, "Restart Updater");
    Controller.prototype._setButtonText(
        buttons.FinishButton,
        "Close Without Updating Atlas"
    );
}

Controller.prototype.onFinishedPageEntered = function()
{
    if (installer.status !== QInstaller.Success) {
        return;
    }

    installer.setValue(
        "FinishedText",
        "<html><body>"
        + "<p><b>Update complete.</b></p>"
        + "<p>If Atlas itself was not updated yet, run MaintenanceTool again to install the remaining Atlas package updates.</p>"
        + "</body></html>"
    );
}
