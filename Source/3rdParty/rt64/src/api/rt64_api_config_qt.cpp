//
// RT64
//

#include "rt64_api_config_qt.h"

#include <fstream>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

#include "rt64_api_common.h"
#include "common/rt64_user_configuration.h"
#include "common/rt64_user_paths.h"
#include "hle/rt64_shared_queue_resources.h"
#include "render/rt64_shader_library.h"

namespace {
    struct ConfigStorage {
        RT64::UserPaths paths;

        ConfigStorage() {
            RT64::ApplicationConfiguration appConfig;
            paths.setupPaths(paths.detectDataPath(appConfig.appId));
        }

        bool load(RT64::UserConfiguration &cfg) {
            if (paths.isEmpty()) {
                return false;
            }

            std::ifstream stream(paths.configurationPath, std::ios_base::in);
            if (!stream.is_open()) {
                return false;
            }

            const bool read = RT64::ConfigurationJSON::read(cfg, stream);
            cfg.validate();
            return read;
        }

        bool save(const RT64::UserConfiguration &cfg) {
            if (paths.isEmpty()) {
                return false;
            }

            std::filesystem::path dirPath(paths.dataPath);
            if (!std::filesystem::is_directory(dirPath) && !std::filesystem::create_directories(dirPath)) {
                return false;
            }

            std::ofstream stream(paths.configurationPath, std::ios_base::out);
            if (!stream.is_open()) {
                return false;
            }

            return RT64::ConfigurationJSON::write(cfg, stream);
        }
    };

    QComboBox *addCombo(QFormLayout *layout, const QString &label, const QStringList &items, int current) {
        QComboBox *combo = new QComboBox();
        combo->addItems(items);
        combo->setCurrentIndex(std::clamp(current, 0, int(items.size()) - 1));
        layout->addRow(label, combo);
        return combo;
    }

    QCheckBox *addCheck(QFormLayout *layout, const QString &label, bool checked) {
        QCheckBox *check = new QCheckBox();
        check->setChecked(checked);
        layout->addRow(label, check);
        return check;
    }

    QDoubleSpinBox *addDouble(QFormLayout *layout, const QString &label, double value, double min, double max, double step) {
        QDoubleSpinBox *spin = new QDoubleSpinBox();
        spin->setRange(min, max);
        spin->setSingleStep(step);
        spin->setDecimals(3);
        spin->setValue(value);
        layout->addRow(label, spin);
        return spin;
    }

    QSpinBox *addInt(QFormLayout *layout, const QString &label, int value, int min, int max) {
        QSpinBox *spin = new QSpinBox();
        spin->setRange(min, max);
        spin->setValue(value);
        layout->addRow(label, spin);
        return spin;
    }

    // Applies the configuration to the running application, mirroring what the inspector does.
    void applyToRunningApplication(const RT64::UserConfiguration &cfg) {
        RT64::Application *app = RT64::API.app.get();
        if ((app == nullptr) || (app->sharedQueueResources == nullptr)) {
            return;
        }

        const RT64::UserConfiguration previous = app->userConfig;
        const bool resolutionChanged =
            (previous.resolution != cfg.resolution) ||
            (previous.resolutionMultiplier != cfg.resolutionMultiplier) ||
            (previous.aspectRatio != cfg.aspectRatio) ||
            (previous.aspectTarget != cfg.aspectTarget);

        app->userConfig = cfg;
        app->userConfig.validate();

        if (app->shaderLibrary != nullptr) {
            app->shaderLibrary->usesHardwareResolve = (cfg.hardwareResolve != RT64::UserConfiguration::HardwareResolve::Disabled);
        }

        if (previous.msaaSampleCount() != app->userConfig.msaaSampleCount()) {
            app->updateMultisampling();
        }

        app->updateUserConfig(resolutionChanged);
    }
};

bool RT64ShowConfigDialog(void *parentWidget) {
    using namespace RT64;

    ConfigStorage storage;
    UserConfiguration cfg;
    if (API.app != nullptr) {
        cfg = API.app->userConfig;
    }
    else {
        storage.load(cfg);
    }

    QDialog dialog(reinterpret_cast<QWidget *>(parentWidget));
    dialog.setWindowTitle("RT64 Settings");
    QVBoxLayout *mainLayout = new QVBoxLayout(&dialog);

    QGroupBox *renderGroup = new QGroupBox("Rendering");
    QFormLayout *renderLayout = new QFormLayout(renderGroup);
    QComboBox *resolutionCombo = addCombo(renderLayout, "Resolution mode", { "Original", "Window integer scale", "Manual" }, int(cfg.resolution));
    QDoubleSpinBox *resolutionMultiplier = addDouble(renderLayout, "Resolution multiplier", cfg.resolutionMultiplier, 0.25, UserConfiguration::ResolutionMultiplierLimit, 0.25);
    QSpinBox *downsampleMultiplier = addInt(renderLayout, "Downsample multiplier", cfg.downsampleMultiplier, 1, UserConfiguration::ResolutionMultiplierLimit);
    QComboBox *antialiasingCombo = addCombo(renderLayout, "Antialiasing", { "None", "MSAA 2X", "MSAA 4X", "MSAA 8X" }, int(cfg.antialiasing));
    QComboBox *filteringCombo = addCombo(renderLayout, "Filtering", { "Nearest", "Linear", "Pixel scaling (anti-aliased)" }, int(cfg.filtering));
    QCheckBox *threePointCheck = addCheck(renderLayout, "Three-point filtering", cfg.threePointFiltering);
    QComboBox *upscale2DCombo = addCombo(renderLayout, "Upscale 2D mode", { "Original", "Scaled only", "All" }, int(cfg.upscale2D));
    QComboBox *colorFormatCombo = addCombo(renderLayout, "Color format (restart required)", { "Standard", "High", "Automatic" }, int(cfg.internalColorFormat));
    QComboBox *hardwareResolveCombo = addCombo(renderLayout, "Hardware resolve", { "Disabled", "Enabled", "Automatic" }, int(cfg.hardwareResolve));
    mainLayout->addWidget(renderGroup);

    QGroupBox *aspectGroup = new QGroupBox("Aspect ratio");
    QFormLayout *aspectLayout = new QFormLayout(aspectGroup);
    QComboBox *aspectCombo = addCombo(aspectLayout, "Aspect ratio mode", { "Original", "Expand", "Manual" }, int(cfg.aspectRatio));
    QDoubleSpinBox *aspectTarget = addDouble(aspectLayout, "Aspect ratio (manual)", cfg.aspectTarget, 0.1, 100.0, 0.01);
    QComboBox *extAspectCombo = addCombo(aspectLayout, "Extended GBI aspect ratio mode", { "Original", "Expand", "Manual" }, int(cfg.extAspectRatio));
    QDoubleSpinBox *extAspectTarget = addDouble(aspectLayout, "Extended GBI aspect ratio (manual)", cfg.extAspectTarget, 0.1, 100.0, 0.01);
    mainLayout->addWidget(aspectGroup);

    QGroupBox *displayGroup = new QGroupBox("Display");
    QFormLayout *displayLayout = new QFormLayout(displayGroup);
    QComboBox *refreshRateCombo = addCombo(displayLayout, "Refresh rate mode (frame interpolation)", { "Original (no interpolation)", "Display", "Manual" }, int(cfg.refreshRate));
    QSpinBox *refreshRateTarget = addInt(displayLayout, "Refresh rate target (manual)", cfg.refreshRateTarget, 10, 1000);
    QComboBox *bufferingCombo = addCombo(displayLayout, "Display buffering", { "Double", "Triple" }, int(cfg.displayBuffering));
    QComboBox *presentWaitCombo = addCombo(displayLayout, "Present wait (restart required)", { "Disabled", "Enabled", "Automatic" }, int(cfg.presentWait));
    QCheckBox *idleWorkCheck = addCheck(displayLayout, "High performance state (idle work)", cfg.idleWorkActive);
    mainLayout->addWidget(displayGroup);

    QGroupBox *developerGroup = new QGroupBox("Developer");
    QFormLayout *developerLayout = new QFormLayout(developerGroup);
    QCheckBox *developerCheck = addCheck(developerLayout, "Developer mode (F10 opens the inspector in-game)", cfg.developerMode);
    mainLayout->addWidget(developerGroup);

    QLabel *pathLabel = new QLabel(QString("Configuration file: %1").arg(QString::fromStdString(storage.paths.configurationPath.string())));
    pathLabel->setWordWrap(true);
    mainLayout->addWidget(pathLabel);

    auto updateEnabled = [&]() {
        resolutionMultiplier->setEnabled(resolutionCombo->currentIndex() == int(UserConfiguration::Resolution::Manual));
        aspectTarget->setEnabled(aspectCombo->currentIndex() == int(UserConfiguration::AspectRatio::Manual));
        extAspectTarget->setEnabled(extAspectCombo->currentIndex() == int(UserConfiguration::AspectRatio::Manual));
        refreshRateTarget->setEnabled(refreshRateCombo->currentIndex() == int(UserConfiguration::RefreshRate::Manual));
    };

    QObject::connect(resolutionCombo, &QComboBox::currentIndexChanged, &dialog, [&](int) { updateEnabled(); });
    QObject::connect(aspectCombo, &QComboBox::currentIndexChanged, &dialog, [&](int) { updateEnabled(); });
    QObject::connect(extAspectCombo, &QComboBox::currentIndexChanged, &dialog, [&](int) { updateEnabled(); });
    QObject::connect(refreshRateCombo, &QComboBox::currentIndexChanged, &dialog, [&](int) { updateEnabled(); });
    updateEnabled();

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults);
    mainLayout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dialog, [&]() {
        UserConfiguration defaults;
        resolutionCombo->setCurrentIndex(int(defaults.resolution));
        resolutionMultiplier->setValue(defaults.resolutionMultiplier);
        downsampleMultiplier->setValue(defaults.downsampleMultiplier);
        antialiasingCombo->setCurrentIndex(int(defaults.antialiasing));
        filteringCombo->setCurrentIndex(int(defaults.filtering));
        threePointCheck->setChecked(defaults.threePointFiltering);
        upscale2DCombo->setCurrentIndex(int(defaults.upscale2D));
        colorFormatCombo->setCurrentIndex(int(defaults.internalColorFormat));
        hardwareResolveCombo->setCurrentIndex(int(defaults.hardwareResolve));
        aspectCombo->setCurrentIndex(int(defaults.aspectRatio));
        aspectTarget->setValue(defaults.aspectTarget);
        extAspectCombo->setCurrentIndex(int(defaults.extAspectRatio));
        extAspectTarget->setValue(defaults.extAspectTarget);
        refreshRateCombo->setCurrentIndex(int(defaults.refreshRate));
        refreshRateTarget->setValue(defaults.refreshRateTarget);
        bufferingCombo->setCurrentIndex(int(defaults.displayBuffering));
        presentWaitCombo->setCurrentIndex(int(defaults.presentWait));
        idleWorkCheck->setChecked(defaults.idleWorkActive);
        developerCheck->setChecked(defaults.developerMode);
    });

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    cfg.resolution = UserConfiguration::Resolution(resolutionCombo->currentIndex());
    cfg.resolutionMultiplier = resolutionMultiplier->value();
    cfg.downsampleMultiplier = downsampleMultiplier->value();
    cfg.antialiasing = UserConfiguration::Antialiasing(antialiasingCombo->currentIndex());
    cfg.filtering = UserConfiguration::Filtering(filteringCombo->currentIndex());
    cfg.threePointFiltering = threePointCheck->isChecked();
    cfg.upscale2D = UserConfiguration::Upscale2D(upscale2DCombo->currentIndex());
    cfg.internalColorFormat = UserConfiguration::InternalColorFormat(colorFormatCombo->currentIndex());
    cfg.hardwareResolve = UserConfiguration::HardwareResolve(hardwareResolveCombo->currentIndex());
    cfg.aspectRatio = UserConfiguration::AspectRatio(aspectCombo->currentIndex());
    cfg.aspectTarget = aspectTarget->value();
    cfg.extAspectRatio = UserConfiguration::AspectRatio(extAspectCombo->currentIndex());
    cfg.extAspectTarget = extAspectTarget->value();
    cfg.refreshRate = UserConfiguration::RefreshRate(refreshRateCombo->currentIndex());
    cfg.refreshRateTarget = refreshRateTarget->value();
    cfg.displayBuffering = UserConfiguration::DisplayBuffering(bufferingCombo->currentIndex());
    cfg.presentWait = UserConfiguration::PresentWait(presentWaitCombo->currentIndex());
    cfg.idleWorkActive = idleWorkCheck->isChecked();
    cfg.developerMode = developerCheck->isChecked();
    cfg.validate();

    const bool saved = storage.save(cfg);
    applyToRunningApplication(cfg);
    return saved;
}
