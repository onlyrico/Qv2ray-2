#include "V2RayKernelInteractions.hpp"

#include "APIBackend.hpp"
#include "core/connection/ConnectionIO.hpp"
#include "utils/QvHelpers.hpp"

#include <QProcess>

#define QV2RAY_GENERATED_FILE_PATH (QV2RAY_GENERATED_DIR + "config.gen.json")
#define QV_MODULE_NAME "V2RayInteraction"

namespace Qv2ray::core::kernel
{
    std::pair<bool, std::optional<QString>> V2RayKernelInstance::ValidateKernel(const QString &corePath, const QString &assetsPath)
    {
        QFile coreFile(corePath);

        if (!coreFile.exists())
            return { false, tr("V2Ray core executable not found.") };

        // Use open() here to prevent `executing` a folder, which may have the
        // same name as the V2Ray core.
        if (!coreFile.open(QFile::ReadOnly))
            return { false, tr("V2Ray core file cannot be opened, please ensure there's a file instead of a folder.") };

        coreFile.close();

        //
        // Check file existance.
        // From: https://www.v2fly.org/chapter_02/env.html#asset-location
        bool hasGeoIP = FileExistsIn(QDir(assetsPath), "geoip.dat");
        bool hasGeoSite = FileExistsIn(QDir(assetsPath), "geosite.dat");

        if (!hasGeoIP && !hasGeoSite)
            return { false, tr("V2Ray assets path is not valid.") };

        if (!hasGeoIP)
            return { false, tr("No geoip.dat in assets path.") };

        if (!hasGeoSite)
            return { false, tr("No geosite.dat in assets path.") };

        // Check if V2Ray core returns a version number correctly.
        QProcess proc;
#ifdef Q_OS_WIN32
        // nativeArguments are required for Windows platform, without a
        // reason...
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.setProgram(corePath);
        proc.setNativeArguments("--version");
        proc.start();
#else
        proc.start(corePath, { "--version" });
#endif
        proc.waitForStarted();
        proc.waitForFinished();
        auto exitCode = proc.exitCode();

        if (exitCode != 0)
            return { false, tr("V2Ray core failed with an exit code: ") + QSTRN(exitCode) };

        const auto output = proc.readAllStandardOutput();
        LOG("V2Ray output: " + SplitLines(output).join(";"));

        if (SplitLines(output).isEmpty())
            return { false, tr("V2Ray core returns empty string.") };

        return { true, SplitLines(output).at(0) };
    }

    std::optional<QString> V2RayKernelInstance::ValidateConfig(const QString &path)
    {
        const auto kernelPath = GlobalConfig.kernelConfig->KernelPath();
        const auto assetsPath = GlobalConfig.kernelConfig->AssetsPath();
        if (const auto &[result, msg] = ValidateKernel(kernelPath, assetsPath); result)
        {
            DEBUG("V2Ray version: " + *msg);
            // Append assets location env.
            auto env = QProcessEnvironment::systemEnvironment();
            env.insert("v2ray.location.asset", assetsPath);
            //
            QProcess process;
            process.setProcessEnvironment(env);
            DEBUG("Starting V2Ray core with test options");
            process.start(kernelPath, { "-test", "-config", path }, QIODevice::ReadWrite | QIODevice::Text);
            process.waitForFinished();

            if (process.exitCode() != 0)
            {
                QString output = QString(process.readAllStandardOutput());
                if (!qEnvironmentVariableIsSet("QV2RAY_ALLOW_XRAY_CORE") && output.contains("Xray, Penetrates Everything."))
                    ((QObject *) nullptr)->event(nullptr);
                QvMessageBoxWarn(nullptr, tr("Configuration Error"), output.mid(output.indexOf("anti-censorship.") + 17));
                return std::nullopt;
            }

            DEBUG("Config file check passed.");
            return std::nullopt;
        }
        else
        {
            return msg;
        }
    }

    V2RayKernelInstance::V2RayKernelInstance(QObject *parent) : QObject(parent)
    {
        vProcess = new QProcess();
        connect(vProcess, &QProcess::readyReadStandardOutput, this,
                [&]() { emit OnProcessOutputReadyRead(vProcess->readAllStandardOutput().trimmed()); });
        connect(vProcess, &QProcess::stateChanged, [&](QProcess::ProcessState state) {
            DEBUG("V2Ray kernel process status changed: " + QVariant::fromValue(state).toString());

            // If V2Ray crashed AFTER we start it.
            if (kernelStarted && state == QProcess::NotRunning)
            {
                LOG("V2Ray kernel crashed.");
                StopConnection();
                emit OnProcessErrored("V2Ray kernel crashed.");
            }
        });
        apiWorker = new APIWorker();
        qRegisterMetaType<StatisticsType>();
        qRegisterMetaType<QMap<StatisticsType, QvStatsSpeed>>();
        connect(apiWorker, &APIWorker::onAPIDataReady, this, &V2RayKernelInstance::OnNewStatsDataArrived);
        kernelStarted = false;
    }

    std::optional<QString> V2RayKernelInstance::StartConnection(const CONFIGROOT &root)
    {
        if (kernelStarted)
        {
            LOG("Status is invalid, expect STOPPED when calling StartConnection");
            return tr("Invalid V2Ray Instance Status.");
        }

        const auto json = JsonToString(root);
        StringToFile(json, QV2RAY_GENERATED_FILE_PATH);
        //
        auto filePath = QV2RAY_GENERATED_FILE_PATH;

        if (const auto &result = ValidateConfig(filePath); result)
        {

            kernelStarted = false;
            return tr("V2Ray kernel failed to start: ") + *result;
        }

        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("v2ray.location.asset", GlobalConfig.kernelConfig->AssetsPath());
        vProcess->setProcessEnvironment(env);
        vProcess->start(GlobalConfig.kernelConfig->KernelPath(), { "-config", filePath }, QIODevice::ReadWrite | QIODevice::Text);
        vProcess->waitForStarted();
        kernelStarted = true;

        QMap<bool, QMap<QString, QString>> tagProtocolMap;
        for (const auto isOutbound : { *GlobalConfig.uiConfig->graphConfig->useOutboundStats, false })
        {
            for (const auto &item : root[isOutbound ? "outbounds" : "inbounds"].toArray())
            {
                const auto tag = item.toObject()["tag"].toString();
                if (tag == API_TAG_INBOUND)
                    continue;
                if (tag.isEmpty())
                {
                    LOG("Ignored inbound with empty tag.");
                    continue;
                }
                tagProtocolMap[isOutbound][tag] = item.toObject()["protocol"].toString();
            }
        }

        apiEnabled = false;
        if (QvCoreApplication->StartupArguments.noAPI)
        {
            LOG("API has been disabled by the command line arguments");
        }
        else if (!GlobalConfig.kernelConfig->enableAPI)
        {
            LOG("API has been disabled by the global config option");
        }
        else if (tagProtocolMap.isEmpty())
        {
            LOG("RARE: API is disabled since no inbound tags configured. This is usually caused by a bad complex config.");
        }
        else
        {
            DEBUG("Starting API");
            apiWorker->StartAPI(tagProtocolMap);
            apiEnabled = true;
        }

        return std::nullopt;
    }

    void V2RayKernelInstance::StopConnection()
    {
        if (apiEnabled)
        {
            apiWorker->StopAPI();
            apiEnabled = false;
        }

        // Set this to false BEFORE close the Process, since we need this flag
        // to capture the real kernel CRASH
        kernelStarted = false;
        vProcess->close();
        // Block until V2Ray core exits
        // Should we use -1 instead of waiting for 30secs?
        vProcess->waitForFinished();
    }

    V2RayKernelInstance::~V2RayKernelInstance()
    {
        if (kernelStarted)
        {
            StopConnection();
        }

        delete apiWorker;
        delete vProcess;
    }

} // namespace Qv2ray::core::kernel
