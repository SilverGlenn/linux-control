# RC1 execution evidence

Candidate `0.1.0-rc.1+7584b7fda4e74a21`, source snapshot on 25 October 2026.

The final Release build passed all 107 isolated suites; 30 focused suites passed. Normal/stripped staging, runtime loader independence, reversible deployment and 32 fixture captures were checked. The exact normal 11-file payload was installed under `/home/siru/.local`; headless identity and payload hashes match.

**Not qualified:** installed GUI/menu launch, W1–W7, modified-provider effects/restoration, and sign-in persistence remain unrun. W2 requires a second real output. No five-day trial has started.

`source-manifest.json` records the immutable build snapshot, including modified/untracked source and Batch 25. The source archive is retained in the local candidate bundle at `build/rc1-bundle-2/source/source.tar.gz`; hash in the summary. These result files and qualification-document updates are published after the build and are not compiled inputs. A final input proof is recorded separately. No private KDE configuration or backup bytes are published here.

Fixture results cannot establish live adoption or persistence. Screenshots use injected providers and a viewport harness at the observed 1× scale; the three images here are representative, with all 32 hashes in the summary. Installed headless `--version` does not pass graphical launch.
