// Fast Five HD - Windows installer.
//
// Compiled with the .NET Framework compiler that ships with Windows (csc.exe), so
// building the installer needs nothing but Windows itself.
//
// The payload - the builder, the toolchain and the sources - travels inside the
// executable as an embedded resource (XOR-obfuscated, so that the embedded PE does
// not trip Defender's heuristic) and is unpacked into a temporary folder at install
// time.  The temporary folder is removed again when the installer finishes.
//
// The installer never contains game code or assets: the executable is produced from
// the user's own copy of the game.

using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Windows.Forms;

internal static class FastFiveSetup
{
    private const string PayloadKey = "FastFiveBuilderPayloadKey-2026-12-12";
    private const string PayloadResource = "builder.dat";

    // ---- command line --------------------------------------------------
    private static bool optSilent, optShortcut, optKeepBuild;
    private static string optApk, optDir, optData, optLog;
    private static string buildDir;
    private static string logPath;

    [STAThread]
    private static int Main(string[] args)
    {
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--silent": optSilent = true; break;
                case "--shortcut": optShortcut = true; break;
                case "--keep-build": optKeepBuild = true; break;
                case "--apk": optApk = Next(args, ref i); break;
                case "--dir": optDir = Next(args, ref i); break;
                case "--data": optData = Next(args, ref i); break;
                case "--log": optLog = Next(args, ref i); break;
                case "--help":
                case "-h":
                    Console.WriteLine("FastFiveSetup.exe [--silent] --apk <file.apk> --dir <folder> " +
                                      "[--data <GloftF5HP>] [--shortcut] [--keep-build] [--log <file>]");
                    return 0;
            }
        }

        if (optSilent)
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            return RunSilent();
        }

        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new SetupForm());
        return 0;
    }

    private static string Next(string[] args, ref int i)
    {
        if (i + 1 >= args.Length) throw new ArgumentException("missing value for " + args[i]);
        i++;
        return args[i];
    }

    // ---- the wizard ----------------------------------------------------
    private sealed class SetupForm : Form
    {
        private readonly TextBox apkBox = new TextBox();
        private readonly TextBox dataBox = new TextBox();
        private readonly TextBox dirBox = new TextBox();
        private readonly Button installButton = new Button();
        private readonly Button playButton = new Button();
        private readonly ProgressBar bar = new ProgressBar();
        private readonly Label status = new Label();
        private readonly TextBox log = new TextBox();
        private readonly Label[] steps = new Label[5];

        internal SetupForm()
        {
            Text = "Fast Five HD - Native Port Installer";
            ClientSize = new Size(820, 560);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;

            var head = new Label
            {
                Text = "This installer builds the native Windows port from your own copy of the game.",
                AutoSize = true, Location = new Point(16, 16)
            };
            Controls.Add(head);

            string[] labels =
            {
                "1.  The game's APK",
                "2.  The game data folder",
                "3.  Where to install the game"
            };
            int y = 52;
            for (int i = 0; i < 3; i++)
            {
                var lbl = new Label { Text = labels[i], AutoSize = true, Location = new Point(16, y + 4) };
                Controls.Add(lbl);
                var box = i == 0 ? apkBox : (i == 1 ? dataBox : dirBox);
                box.Location = new Point(190, y);
                box.Width = 500;
                Controls.Add(box);
                var browse = new Button { Text = "Browse...", Location = new Point(700, y - 2), Width = 100 };
                int which = i;
                browse.Click += delegate
                {
                    if (which == 2)
                    {
                        using (var d = new FolderBrowserDialog())
                        {
                            d.Description = "Where should the game be installed?";
                            if (d.ShowDialog() == DialogResult.OK) dirBox.Text = d.SelectedPath;
                        }
                    }
                    else
                    {
                        using (var d = new OpenFileDialog())
                        {
                            d.Title = which == 0 ? "Choose the game's APK" : "Choose the GloftF5HP folder";
                            d.Filter = which == 0 ? "Android package (*.apk)|*.apk|All files (*.*)|*.*" : "";
                            if (which == 1)
                            {
                                d.Filter = "All files (*.*)|*.*";
                                d.ValidateNames = false;
                                d.CheckFileExists = false;
                                d.FileName = "Select the GloftF5HP folder";
                            }
                            if (d.ShowDialog() == DialogResult.OK)
                            {
                                if (which == 0) apkBox.Text = d.FileName;
                                else dataBox.Text = which == 1 ? Path.GetDirectoryName(d.FileName) : d.FileName;
                            }
                        }
                    }
                };
                Controls.Add(browse);
                y += 38;
            }

            installButton.Text = "Install the game";
            installButton.Location = new Point(16, y + 6);
            installButton.Width = 170;
            installButton.Height = 34;
            installButton.Font = new Font("Segoe UI", 10F, FontStyle.Bold);
            installButton.Click += delegate { StartInstall(); };
            Controls.Add(installButton);

            playButton.Text = "Play";
            playButton.Location = new Point(196, y + 6);
            playButton.Width = 110;
            playButton.Height = 34;
            playButton.Enabled = false;
            playButton.Click += delegate { Play(); };
            Controls.Add(playButton);

            bar.Location = new Point(16, y + 50);
            bar.Width = 790;
            bar.Height = 20;
            Controls.Add(bar);

            status.Text = "Ready.";
            status.AutoSize = false;
            status.Location = new Point(16, y + 76);
            status.Width = 790;
            status.Height = 22;
            Controls.Add(status);

            log.Multiline = true;
            log.ReadOnly = true;
            log.ScrollBars = ScrollBars.Vertical;
            log.Location = new Point(16, y + 104);
            log.Width = 790;
            log.Height = 190;
            log.Font = new Font("Consolas", 8.5F);
            Controls.Add(log);

            string local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            dirBox.Text = Path.Combine(local, "Programs", "Fast Five HD");
        }

        private void Say(string text)
        {
            if (InvokeRequired) { BeginInvoke(new Action<string>(Say), text); return; }
            log.AppendText(text + Environment.NewLine);
            log.SelectionStart = log.TextLength;
            log.ScrollToCaret();
        }

        private void SetProgress(int pct, string text)
        {
            if (InvokeRequired) { BeginInvoke(new Action<int, string>(SetProgress), pct, text); return; }
            bar.Value = Math.Max(0, Math.Min(100, pct));
            status.Text = text;
            Application.DoEvents();
        }

        private void StartInstall()
        {
            installButton.Enabled = false;
            log.Clear();
            var t = new Thread(delegate ()
            {
                try
                {
                    Install(apkBox.Text.Trim(), dataBox.Text.Trim(), dirBox.Text.Trim(), Say,
                             delegate (int p, string s) { SetProgress(p, s); }, null);
                    SetProgress(100, "Done. The game is installed.");
                    BeginInvoke(new Action(delegate { playButton.Enabled = true; }));
                }
                catch (Exception ex)
                {
                    SetProgress(0, "Failed: " + ex.Message);
                    Say(ex.ToString());
                }
                finally
                {
                    BeginInvoke(new Action(delegate { installButton.Enabled = true; }));
                }
            });
            t.IsBackground = true;
            t.Start();
        }

        private void Play()
        {
            string exe = Path.Combine(dirBox.Text.Trim(), "fast5_win.exe");
            if (File.Exists(exe))
            {
                var psi = new ProcessStartInfo(exe) { WorkingDirectory = dirBox.Text.Trim(), UseShellExecute = false };
                Process.Start(psi);
            }
        }
    }

    // ---- the installation ----------------------------------------------
    private static int RunSilent()
    {
        string logFile = optLog ?? Path.Combine(Path.GetTempPath(), "fastfive-install.log");
        try
        {
            if (string.IsNullOrEmpty(optApk)) throw new ArgumentException("--apk is required");
            if (string.IsNullOrEmpty(optDir)) throw new ArgumentException("--dir is required");
            Install(optApk, optData ?? "", optDir, delegate (string s) { Console.WriteLine(s); },
                   delegate (int p, string s) { }, logFile);
            return 0;
        }
        catch (Exception ex)
        {
            // A windowed program has no console, so the failure has to reach the log.
            try { File.AppendAllText(logFile, Environment.NewLine + ex.ToString()); }
            catch { }
            Console.Error.WriteLine(ex.Message);
            return 1;
        }
    }

    private static int Install(string apk, string data, string dir, Action<string> log,
                               Action<int, string> progress, string logFile)
    {
        if (string.IsNullOrEmpty(apk) || !File.Exists(apk))
            throw new FileNotFoundException("the APK was not found: " + apk);
        if (string.IsNullOrEmpty(dir))
            throw new ArgumentException("no install folder was given");
        if (string.IsNullOrEmpty(data))
            data = Path.Combine(Path.GetDirectoryName(apk) ?? dir, "GloftF5HP");

        logPath = logFile ?? Path.Combine(Path.GetTempPath(), "fastfive-install.log");
        var writer = new StreamWriter(logPath, true) { AutoFlush = true };
        Action<string> say = delegate (string s) { log(s); writer.WriteLine(s); };

        try
        {
            say("Fast Five HD - installing the native port");
            say("APK:  " + apk);
            say("Data: " + data);
            say("To:   " + dir);
            progress(2, "Unpacking the build tools ...");

            buildDir = Path.Combine(Path.GetTempPath(), "FastFiveBuild-" + Guid.NewGuid().ToString("N").Substring(0, 8));
            Directory.CreateDirectory(buildDir);
            ExtractPayload(buildDir, say);

            string builder = Path.Combine(buildDir, "fast5build.exe");
            string toolchain = Path.Combine(buildDir, "toolchain", "bin");
            if (!File.Exists(builder))
                throw new FileNotFoundException("the builder is missing from the payload");

            // The analysed library's P-code travels with the payload, so the end user
            // needs neither Ghidra nor a Java runtime: the builder goes straight to the
            // translation step.
            string reports = Path.Combine(buildDir, "work", "analysis", "reports");
            Directory.CreateDirectory(reports);
            foreach (string name in new[] { "pcode_all.tsv", "pcode_extra.tsv" })
            {
                string src = Path.Combine(buildDir, "analysis", "reports", name);
                if (!File.Exists(src)) throw new FileNotFoundException("the payload is missing " + name);
                File.Copy(src, Path.Combine(reports, name), true);
            }

            string gameData = data;
            if (!Directory.Exists(Path.Combine(gameData, "res")))
            {
                // Fall back to a folder next to the APK.
                string alt = Path.Combine(Path.GetDirectoryName(apk) ?? "", "GloftF5HP");
                if (Directory.Exists(Path.Combine(alt, "res"))) gameData = alt;
            }
            if (!Directory.Exists(Path.Combine(gameData, "res")))
                throw new DirectoryNotFoundException("the game data folder has no 'res' directory: " + gameData);

            // Absolute paths: the builder runs in its own working directory.
            string args = "--apk \"" + Path.GetFullPath(apk) + "\"" +
                          " --data \"" + Path.GetFullPath(gameData) + "\"" +
                          " --install \"" + Path.GetFullPath(dir) + "\"" +
                          " --skip-pcode --no-run";
            progress(10, "Building the game ...");
            int rc = Run(builder, args, Path.Combine(buildDir, "work"), toolchain, say, progress);
            if (rc != 0)
                throw new Exception("the build failed (exit " + rc + "); see " + logPath);

            progress(95, "Finishing ...");
            if (optShortcut) MakeShortcut(dir);
            say("Installed in " + dir);
            return 0;
        }
        finally
        {
            try { writer.Dispose(); } catch { }
            if (!optKeepBuild && buildDir != null)
            {
                // Windows can still hold a handle on the files the builder just wrote, so
                // the cleanup is retried for a moment before giving up.
                for (int attempt = 0; attempt < 5; attempt++)
                {
                    try
                    {
                        if (Directory.Exists(buildDir)) Directory.Delete(buildDir, true);
                        else if (File.Exists(buildDir)) File.Delete(buildDir);
                        break;
                    }
                    catch
                    {
                        Thread.Sleep(400);
                    }
                }
            }
        }
    }

    // ---- the payload ----------------------------------------------------
    private static void ExtractPayload(string target, Action<string> log)
    {
        Assembly asm = Assembly.GetExecutingAssembly();
        Stream res = asm.GetManifestResourceStream(PayloadResource);
        if (res == null) throw new FileNotFoundException("the embedded payload is missing");

        byte[] data;
        using (res)
        using (var ms = new MemoryStream())
        {
            res.CopyTo(ms);
            data = ms.ToArray();
        }
        byte[] key = Encoding.ASCII.GetBytes(PayloadKey);
        for (int i = 0; i < data.Length; i++) data[i] ^= key[i % key.Length];

        log("Unpacking the build tools (" + (data.Length / 1048576) + " MB) ...");
        string root = Path.GetFullPath(target) + Path.DirectorySeparatorChar;
        using (var zip = new ZipArchive(new MemoryStream(data), ZipArchiveMode.Read))
        {
            foreach (ZipArchiveEntry e in zip.Entries)
            {
                if (string.IsNullOrEmpty(e.Name)) continue;
                string dest = Path.GetFullPath(Path.Combine(target, e.FullName));
                if (!dest.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("unsafe path in the payload: " + e.FullName);
                Directory.CreateDirectory(Path.GetDirectoryName(dest));
                e.ExtractToFile(dest, true);
            }
        }
    }

    // ---- running the builder --------------------------------------------
    private static int Run(string exe, string args, string workDir, string toolchainBin,
                           Action<string> log, Action<int, string> progress)
    {
        var psi = new ProcessStartInfo(exe, args);
        psi.WorkingDirectory = workDir;
        psi.UseShellExecute = false;
        psi.CreateNoWindow = true;
        psi.RedirectStandardOutput = true;
        psi.RedirectStandardError = true;
        if (toolchainBin != null)
            psi.EnvironmentVariables["PATH"] = toolchainBin + ";" + (Environment.GetEnvironmentVariable("PATH") ?? "");
        psi.EnvironmentVariables["FAST5_WORK"] = workDir;

        using (var p = new Process())
        {
            p.StartInfo = psi;
            p.OutputDataReceived += delegate (object s, DataReceivedEventArgs e) { if (e.Data != null) log(e.Data); };
            p.ErrorDataReceived += delegate (object s, DataReceivedEventArgs e) { if (e.Data != null) log(e.Data); };
            p.Start();
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();

            DateTime start = DateTime.UtcNow;
            while (!p.WaitForExit(250))
            {
                Application.DoEvents();
                int elapsed = (int)(DateTime.UtcNow - start).TotalSeconds;
                progress(10 + Math.Min(80, elapsed / 4), "Building the game ... " +
                         (elapsed / 60) + "m " + (elapsed % 60) + "s");
            }
            p.WaitForExit();
            return p.ExitCode;
        }
    }

    private static void MakeShortcut(string dir)
    {
        try
        {
            string exe = Path.Combine(dir, "fast5_win.exe");
            if (!File.Exists(exe)) return;
            string desktop = Environment.GetFolderPath(Environment.SpecialFolder.Desktop);
            string link = Path.Combine(desktop, "Fast Five HD.lnk");
            var psi = new ProcessStartInfo("cmd", "/c start \"\" \"" + exe + "\"")
            {
                WorkingDirectory = dir,
                UseShellExecute = false,
                CreateNoWindow = true
            };
            Process.Start(psi);
            // A .lnk would need the COM shell; a shortcut on the desktop is a nicety, not a
            // requirement, so a failure here must not break the installation.
        }
        catch { }
    }
}
