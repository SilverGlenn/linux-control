#pragma once

#include <QObject>
#include <QString>
#include <QDateTime>

class QFutureWatcherBase;
template <typename T> class QFutureWatcher;

// One full Windows-Experience-Index assessment: the five raw metrics, the five
// derived subscores, and the base score (the minimum of the subscores).
struct WeiResult {
    // Raw metrics (the calibration-grade, scale-independent numbers)
    double memoryBandwidthMBs = 0.0;  // streaming copy, one-direction MiB/s (WinSAT convention)
    double cpuThroughputMBs   = 0.0;  // compression + encryption, all cores
    double diskReadMBs        = 0.0;  // sequential read
    double graphicsFps        = 0.0;  // 2D / Aero compositing
    double gamingFps          = 0.0;  // 3D scene

    // Subscores (1.0-7.9; 0.0 means "not assessed")
    double memoryScore   = 0.0;
    double cpuScore      = 0.0;
    double diskScore     = 0.0;
    double graphicsScore = 0.0;
    double gamingScore   = 0.0;
    double baseScore     = 0.0;       // min of the assessed subscores

    QDateTime assessedAt;
    bool valid = false;

    // Recompute subscores + base from the raw metrics via the scoring tables.
    void rescore();

    // Persist to / load from the on-disk data store (our DataStore analogue).
    bool save() const;
    static WeiResult load();
    static QString storePath();
};

// Runs the assessment. CPU/memory/disk run on a worker thread (QtConcurrent);
// the OpenGL graphics/gaming passes run on the GUI thread between them, so the
// whole thing is driven as a small state machine that keeps the UI responsive
// and emits progress as it goes.
class WeiBenchmark : public QObject {
    Q_OBJECT
public:
    // The engine outlives individual pages so navigation cannot discard an
    // active assessment or its latest progress/result.
    static WeiBenchmark *instance();

    explicit WeiBenchmark(QObject *parent = nullptr);
    ~WeiBenchmark() override;

    void run();                 // start a fresh assessment
    bool isRunning() const { return m_running; }
    bool hasResult() const { return m_lastResult.valid; }
    WeiResult result() const { return m_lastResult; }
    int percent() const { return m_percent; }
    QString phaseText() const { return m_phaseText; }

signals:
    void progress(int percent, const QString &phase);
    void stateChanged();
    void finished(const WeiResult &result);

private:
    void setProgress(int percent, const QString &phase);
    void startPhase();          // advance the state machine
    void runWorkerPhase();      // dispatch the current CPU/mem/disk metric
    void runGlPhase();          // run the current GL metric inline

    // Individual benchmarks (return the raw metric)
    static double benchMemory();          // MB/s, r+w convention
    static double benchCpu();             // MB/s aggregate
    static double benchDisk(QString *err);// MB/s sequential read
    double benchGraphics();               // fps (GUI thread)
    double benchGaming();                 // fps (GUI thread)

    enum class Phase { Idle, Cpu, Memory, Disk, Graphics, Gaming, Done };
    Phase m_phase = Phase::Idle;
    bool  m_running = false;
    int m_percent = 0;
    QString m_phaseText;
    WeiResult m_lastResult;
    WeiResult m_workingResult;
    QFutureWatcher<double> *m_watcher = nullptr;

#ifdef WEI_BENCHMARK_TESTING
public:
    // Lifecycle test hooks update the same public state as the real runner
    // without launching CPU, disk, or graphics work.
    void beginTestRun(int percent, const QString &phase);
    void finishTestRun(const WeiResult &result);
#endif
};
