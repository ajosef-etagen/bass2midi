#include "MainComponent.h"

#include <juce_gui_basics/juce_gui_basics.h>

class Bass2MidiApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Bass2MIDI"; }
    const juce::String getApplicationVersion() override { return BASS2MIDI_VERSION; }
    bool moreThanOneInstanceAllowed() override { return false; } // one instance owns the virtual port

    void initialise (const juce::String&) override
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "Bass2MIDI";
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        options.folderName = "Bass2MIDI";
        properties.setStorageParameters (options);

        mainWindow = std::make_unique<MainWindow> (*properties.getUserSettings());
    }

    void shutdown() override
    {
        mainWindow.reset();
        properties.closeFiles();
    }

    void systemRequestedQuit() override { quit(); }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (juce::PropertiesFile& settings)
            : DocumentWindow ("Bass2MIDI",
                              juce::Desktop::getInstance().getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::closeButton | DocumentWindow::minimiseButton)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent (settings), true);
            setResizable (true, false);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    juce::ApplicationProperties properties;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (Bass2MidiApplication)
