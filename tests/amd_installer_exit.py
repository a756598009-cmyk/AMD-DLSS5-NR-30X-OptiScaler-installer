"""Exercise the shipped launchers with Windows PowerShell 5.1 in temp folders.

No release is repackaged and no real game/author binary is used. Only the
fixture installer's accepted runtime SHA is changed for its success cases.
The batch pause gets an output marker because its own text is localized.
"""
from pathlib import Path
import hashlib
import os
import re
import struct
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]
PS = Path(os.environ.get("SystemRoot", r"C:\Windows")) / (
    "System32/WindowsPowerShell/v1.0/powershell.exe"
)
PAUSE_MARKER = "__LAUNCHER_PAUSE__"
FAKE_RUNTIME = b"Harmless installer test data; not an executable."


def write_ps(path, source):
    path.write_bytes(b"\xef\xbb\xbf" + source.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8"))


def embedded_pe(payload):
    """PE headers and one section of data, nothing runnable: only its extent and hash matter."""
    blob = bytearray(0x400)
    blob[:2] = b"MZ"
    struct.pack_into("<I", blob, 0x3C, 0x40)
    blob[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", blob, 0x44, 0x8664, 1, 0, 0, 0, 0, 0x2022)
    blob[0x58:0x60] = b".data\0\0\0"
    struct.pack_into("<IIII", blob, 0x60, 0x200, 0x1000, 0x200, 0x200)
    blob[0x200:0x200 + len(payload)] = payload
    return bytes(blob)


@unittest.skipUnless(os.name == "nt" and PS.exists(), "requires Windows PowerShell 5.1")
class InstallerExitTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="amd-installer-exit-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.package = self.root / "package with spaces"
        self.game = self.root / "game with spaces"
        self.package.mkdir()
        self.game.mkdir()
        self.env = {key.upper(): value for key, value in os.environ.items()}
        # A parent pwsh may export its own PSModulePath. Test the actual PS5
        # runtime with its default module discovery, like a double-click.
        self.env.pop("PSMODULEPATH", None)
        self.env["PATH"] = str(PS.parent) + os.pathsep + self.env.get("PATH", "")
        # NeuralHub's own test-mode store: keeps an installed NeuralHub (HKCU, %LOCALAPPDATA%) out of the tests.
        self.hub_store = self.root / "neuralhub store"
        self.env["NEURALHUB_STORE"] = str(self.hub_store)
        source = (REPO / "tools/PACKAGE_RELEASE.ps1").read_text(encoding="utf-8-sig")
        for title_name, script, file_name in (
            ("Setup", "install", "Setup"),
            ("Uninstall", "uninstall", "Uninstall_OptiScaler_NR"),
        ):
            body = re.search(
                r"@'\n(@echo off\nsetlocal\ntitle OptiScaler AMD pre-SR "
                + title_name + r"\n.*?)\n'@ \| Set-Content", source, re.S
            )
            self.assertIsNotNone(body, f"missing production {file_name}.bat template")
            batch = body.group(1)
            self.assertEqual(len(re.findall(r"(?m)^pause$", batch)), 1)
            batch = batch.replace("\npause\n", f"\necho {PAUSE_MARKER}\npause\n")
            (self.package / f"{file_name}.bat").write_bytes(batch.replace("\n", "\r\n").encode("ascii"))
            original = (REPO / f"tools/{script}-amd-presr.ps1").read_text(encoding="utf-8-sig")
            write_ps(self.package / f"{file_name}.ps1", original)

    def launcher_files(self, name):
        if name == "Uninstall":
            return "Uninstall_OptiScaler_NR.bat", "Uninstall_OptiScaler_NR.ps1"
        return f"{name}.bat", f"{name}.ps1"

    def run_process(self, args, stdin=""):
        result = subprocess.run(
            args, input=stdin.encode("ascii"), stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, cwd=self.package, env=self.env, timeout=30,
        )
        return result.returncode, result.stdout.decode("mbcs", errors="replace")

    def run_batch(self, name="Setup", game=None, proxy="dxgi.dll", stdin=""):
        bat, _ = self.launcher_files(name)
        args = [str(self.package / bat), str(game or self.game)]
        if name == "Setup":
            args.append(proxy)
        # cmd's /c outer quoting is separate from each pathname's quoting.
        command = subprocess.list2cmdline(args)
        code, output = self.run_process('cmd.exe /d /s /c "' + command + '"', stdin)
        self.assertEqual(output.count(PAUSE_MARKER), 1, output)
        self.assertNotIn("Press any key to exit...", output)
        return code, output

    def run_direct(self, name="Setup", game=None, flags=("-NonInteractive",)):
        _, script = self.launcher_files(name)
        args = [str(PS), "-NoProfile", "-ExecutionPolicy", "Bypass", "-STA", "-File",
                str(self.package / script), "-GameDir", str(game or self.game)]
        if name == "Setup":
            args += ["-Proxy", "dxgi.dll"]
        code, output = self.run_process(args + list(flags))
        self.assertNotIn("Press any key to exit...", output)
        return code, output

    def ready_install(self):
        script = self.package / "Setup.ps1"
        source = script.read_text(encoding="utf-8-sig")
        digest = hashlib.sha256(FAKE_RUNTIME).hexdigest().upper()
        source, count = re.subn(r"(\$expectedA030\s*=\s*)'[A-F0-9]{64}'", r"\g<1>'" + digest + "'", source)
        self.assertEqual(count, 1, "success fixture must replace only its own trusted SHA")
        write_ps(script, source)
        (self.package / "OptiScaler.dll").write_bytes(b"fixture proxy")
        (self.package / "version.dll").write_bytes(FAKE_RUNTIME)
        (self.package / "dlssnr_on_amd_weights.bin").write_bytes(bytes(1024 * 1024))

    def broken_author_setup(self):
        (self.package / "OptiScaler.dll").write_bytes(b"fixture proxy")
        (self.package / "dlssnr_on_amd_setup.exe").write_bytes(b"invalid executable")

    def test_install_success_pauses_once(self):
        self.ready_install()
        code, output = self.run_batch()
        self.assertEqual(code, 0, output)
        self.assertIn("Install SUCCEEDED.", output)
        self.assertNotIn("Setup failed", output)
        self.assertEqual((self.game / "dlssnr_amd_pass1.dll").read_bytes(), FAKE_RUNTIME)
        self.assertTrue((self.game / "Uninstall_OptiScaler_NR.bat").is_file(), output)
        self.assertTrue((self.game / "Uninstall_OptiScaler_NR.ps1").is_file(), output)

    def test_explicit_install_failure_pauses_once(self):
        code, output = self.run_batch(game=self.root / "missing")
        self.assertEqual(code, 1, output)
        self.assertIn("Install FAILED.", output)
        self.assertIn("Setup failed (exit code 1)", output)

    def test_invalid_author_setup_is_caught_and_pauses_once(self):
        self.broken_author_setup()
        code, output = self.run_batch()
        self.assertEqual(code, 1, output)
        self.assertIn("Unexpected install error:", output)
        self.assertIn("Install FAILED.", output)
        self.assertIn("Setup failed (exit code 1)", output)

    def test_parameter_binding_failure_has_batch_fallback(self):
        code, output = self.run_batch(proxy="unsupported.dll")
        self.assertNotEqual(code, 0, output)
        self.assertIn(f"Setup failed (exit code {code})", output)
        self.assertNotIn("Install SUCCEEDED.", output)

    def test_batch_preserves_nonstandard_exit_code(self):
        write_ps(self.package / "Setup.ps1", "param($GameDir, $Proxy, [switch]$NoPause)\nexit 23\n")
        code, output = self.run_batch()
        self.assertEqual(code, 23, output)
        self.assertIn("Setup failed (exit code 23)", output)

    def test_cancel_keeps_confirmation_and_is_not_success(self):
        self.ready_install()
        (self.game / "dxgi.dll").write_bytes(b"another mod")
        code, output = self.run_batch(stdin="1\n")
        self.assertEqual(code, 0, output)
        self.assertIn("Cancelled.", output)
        self.assertNotIn("Install SUCCEEDED.", output)
        self.assertNotIn("Setup failed", output)
        self.assertEqual((self.game / "dxgi.dll").read_bytes(), b"another mod")

    def test_noninteractive_install_success_does_not_wait(self):
        self.ready_install()
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("Install SUCCEEDED.", output)

    def test_runtime_is_taken_out_of_setup_without_running_it(self):
        # Since 0.3.3 the author's setup carries version.dll inside itself. Setup takes it out by
        # its PE headers and SHA and must not start the setup, which here is not a program at all.
        blob = embedded_pe(b"Harmless embedded runtime data; not an executable.")
        script = self.package / "Setup.ps1"
        source = script.read_text(encoding="utf-8-sig")
        digest = hashlib.sha256(blob).hexdigest().upper()
        source, count = re.subn(r"(\$expectedA030\s*=\s*)'[A-F0-9]{64}'", r"\g<1>'" + digest + "'", source)
        self.assertEqual(count, 1, "fixture must replace only its own trusted SHA")
        write_ps(script, source)
        (self.package / "OptiScaler.dll").write_bytes(b"fixture proxy")
        (self.package / "dlssnr_on_amd_weights.bin").write_bytes(bytes(1024 * 1024))
        decoy = embedded_pe(b"an image whose hash is not trusted")
        (self.package / "dlssnr_on_amd_setup.exe").write_bytes(
            b"MZ fixture setup" + bytes(48) + decoy + blob + b"[DlssNrOnAmd]\nEnabled=1\n")
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("out of dlssnr_on_amd_setup.exe (not run)", output)
        self.assertNotIn("Launching danielblnc setup", output)
        self.assertEqual((self.game / "dlssnr_amd_pass1.dll").read_bytes(), blob)
        self.assertEqual((self.package / "version.dll").read_bytes(), blob)

    def trust(self, **digests):
        """Replace the named trusted runtime SHAs ($expectedA030 / A031 / A040 / A041 / A080) in the fixture Setup."""
        script = self.package / "Setup.ps1"
        source = script.read_text(encoding="utf-8-sig")
        for var, data in digests.items():
            digest = hashlib.sha256(data).hexdigest().upper()
            source, count = re.subn(r"(\$expected" + var + r"\s*=\s*)'[A-F0-9]{64}'", r"\g<1>'" + digest + "'", source)
            self.assertEqual(count, 1, f"fixture must replace exactly one ${var} SHA")
        write_ps(script, source)
        (self.package / "OptiScaler.dll").write_bytes(b"fixture proxy")
        (self.package / "dlssnr_on_amd_weights.bin").write_bytes(bytes(1024 * 1024))

    def author_setup(self, blob):
        (self.package / "dlssnr_on_amd_setup.exe").write_bytes(
            b"MZ fixture setup" + bytes(48) + blob + b"[DlssNrOnAmd]\nEnabled=1\n")

    def test_newer_runtime_in_setup_wins_over_saved_version_dll(self):
        # Updating from 0.3.1: the earlier install left its version.dll next to Setup.bat and the
        # user dropped the new setup beside it. The setup's newer runtime must be installed, and
        # the package copy updated with the old one kept under its version name.
        old = FAKE_RUNTIME
        new = embedded_pe(b"Harmless newer embedded runtime; not an executable.")
        self.trust(A030=old, A041=new)
        (self.package / "version.dll").write_bytes(old)
        self.author_setup(new)
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("holds runtime 0.4.1, newer than the 0.3.0", output)
        self.assertIn("danielblnc runtime 0.4.1 (SHA256", output)
        self.assertNotIn("Launching danielblnc setup", output)
        self.assertEqual((self.game / "dlssnr_amd_pass1.dll").read_bytes(), new)
        self.assertEqual((self.package / "version.dll").read_bytes(), new)
        self.assertEqual((self.package / "version-0.3.0.dll").read_bytes(), old)

    def test_older_runtime_in_setup_does_not_downgrade(self):
        new = FAKE_RUNTIME
        old = embedded_pe(b"Harmless older embedded runtime; not an executable.")
        self.trust(A030=old, A041=new)
        (self.package / "version.dll").write_bytes(new)
        self.author_setup(old)
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("holds runtime 0.3.0, older than the 0.4.1", output)
        self.assertEqual((self.game / "dlssnr_amd_pass1.dll").read_bytes(), new)
        self.assertEqual((self.package / "version.dll").read_bytes(), new)
        self.assertFalse(list(self.package.glob("version-*.dll")))

    def test_newest_runtime_found_wins_without_setup(self):
        # No setup: a 0.3.0 version.dll next to Setup.bat must not shadow the 0.4.1 pass1 an
        # earlier install put in the game, and must not be overwritten while staging.
        old = FAKE_RUNTIME
        new = b"Harmless newer runtime test data; not an executable."
        self.trust(A030=old, A041=new)
        (self.package / "version.dll").write_bytes(old)
        (self.game / "dlssnr_amd_pass1.dll").write_bytes(new)
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("Found danielblnc runtime 0.3.0", output)
        self.assertIn("Found danielblnc runtime 0.4.1", output)
        self.assertIn("danielblnc runtime 0.4.1 (SHA256", output)
        self.assertEqual((self.game / "dlssnr_amd_pass1.dll").read_bytes(), new)
        self.assertEqual((self.package / "version.dll").read_bytes(), new)
        self.assertEqual((self.package / "version-0.3.0.dll").read_bytes(), old)

    def test_failed_install_removes_the_runtime_taken_out_of_setup(self):
        new = embedded_pe(b"Harmless newer embedded runtime; not an executable.")
        self.trust(A041=new)
        self.author_setup(new)
        (self.package / "Uninstall_OptiScaler_NR.ps1").unlink()
        temp = self.root / "temp"
        temp.mkdir()
        self.env["TEMP"] = self.env["TMP"] = str(temp)
        code, output = self.run_direct()
        self.assertEqual(code, 1, output)
        self.assertIn("out of dlssnr_on_amd_setup.exe (not run)", output)
        self.assertIn("Install FAILED.", output)
        self.assertFalse(list(temp.glob("amd-presr-version-*")), output)

    def test_explicit_author_dll_is_kept_with_a_warning(self):
        old = FAKE_RUNTIME
        new = embedded_pe(b"Harmless newer embedded runtime; not an executable.")
        self.trust(A030=old, A041=new)
        chosen = self.root / "chosen" / "version.dll"
        chosen.parent.mkdir()
        chosen.write_bytes(old)
        self.author_setup(new)
        code, output = self.run_direct(flags=("-NonInteractive", "-AuthorDll", str(chosen)))
        self.assertEqual(code, 0, output)
        self.assertIn("WARNING: dlssnr_on_amd_setup.exe holds runtime 0.4.1, newer than the 0.3.0 given by -AuthorDll",
                      output)
        self.assertIn("NOTE: runtime 0.3.0 is supported, but 0.8.0 (NeuralHub) is recommended", output)
        self.assertEqual((self.game / "dlssnr_amd_pass1.dll").read_bytes(), old)

    def test_unknown_author_dll_does_not_hold_back_a_newer_setup(self):
        old = FAKE_RUNTIME
        new = embedded_pe(b"Harmless newer embedded runtime; not an executable.")
        self.trust(A030=old, A041=new)
        (self.package / "version.dll").write_bytes(old)
        unknown = self.root / "unknown.dll"
        unknown.write_bytes(b"not a known runtime")
        self.author_setup(new)
        code, output = self.run_direct(flags=("-NonInteractive", "-AuthorDll", str(unknown)))
        self.assertEqual(code, 0, output)
        self.assertIn("holds runtime 0.4.1, newer than the 0.3.0", output)
        self.assertEqual((self.game / "dlssnr_amd_pass1.dll").read_bytes(), new)

    def nvngx(self, version, name="nvngx_dlssnr.dll", where=None):
        """A harmless .NET library whose version resource says it is nvngx_dlssnr.dll <version>, padded past 1 MB."""
        path = (where or self.package) / name
        path.parent.mkdir(parents=True, exist_ok=True)
        source = ('using System.Reflection;\n[assembly: AssemblyFileVersion("' + version + '")]\n'
                  'public class NvngxVersionFixture { }\n')
        script = ("Add-Type -TypeDefinition $env:FIXTURE_SOURCE -OutputAssembly $env:FIXTURE_OUT "
                  "-OutputType Library")
        env = dict(self.env, FIXTURE_SOURCE=source, FIXTURE_OUT=str(path))
        result = subprocess.run([str(PS), "-NoProfile", "-NonInteractive", "-Command", script],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout.decode("mbcs", errors="replace"))
        with open(path, "ab") as f:
            f.write(bytes(1024 * 1024))
        return path

    def neuralhub_exe(self, runtime, path=None):
        """neuralhub.exe holds four images; only the runtime's hash is trusted."""
        path = path or (self.package / "neuralhub.exe")
        path.write_bytes(
            b"MZ fixture manager" + bytes(46) + embedded_pe(b"stub: NeuralHubStub_Abi")
            + embedded_pe(b"fixture FidelityFX upscaler") + runtime
            + embedded_pe(b"fixture FidelityFX frame generation") + b"tail")
        return path

    def trust_080(self, runtime, **others):
        self.trust(A080=runtime, **others)
        (self.package / "dlssnr_on_amd_weights.bin").unlink()

    def test_080_from_neuralhub_exe_installs_one_module(self):
        # The manager carries the runtime raw next to three other images. 0.8.0 is a single module:
        # pass1 only, and a pass2/pass3 an earlier install left are removed.
        old = FAKE_RUNTIME
        new = embedded_pe(b"Harmless 0.8.0 runtime fixture; not an executable.")
        self.trust(A041=old, A080=new)
        (self.package / "version.dll").write_bytes(old)
        for p in (1, 2, 3):
            (self.game / f"dlssnr_amd_pass{p}.dll").write_bytes(old)
        self.neuralhub_exe(new)
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("neuralhub.exe holds runtime 0.8.0, newer than the 0.4.1", output)
        self.assertIn("Took danielblnc runtime 0.8.0 out of neuralhub.exe (not run)", output)
        self.assertIn("danielblnc runtime 0.8.0 (SHA256", output)
        self.assertNotIn("is recommended", output)
        self.assertIn("removed stale dlssnr_amd_pass2.dll", output)
        self.assertEqual((self.game / "dlssnr_amd_pass1.dll").read_bytes(), new)
        self.assertFalse((self.game / "dlssnr_amd_pass2.dll").exists(), output)
        self.assertFalse((self.game / "dlssnr_amd_pass3.dll").exists(), output)
        self.assertTrue((self.game / "dlssnr_on_amd_weights.bin").is_file(), output)
        self.assertEqual((self.package / "version.dll").read_bytes(), new)
        self.assertEqual((self.package / "version-0.4.1.dll").read_bytes(), old)

    def test_080_without_weights_takes_nvngx_for_optiscaler(self):
        runtime = b"Harmless 0.8.0 runtime fixture; not an executable."
        self.trust_080(runtime)
        (self.package / "neuralhub_host.dll").write_bytes(runtime)
        nv = self.nvngx("310.8.0.0")
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("Found danielblnc runtime 0.8.0", output)
        self.assertIn("builds it from nvngx_dlssnr.dll on the first launch", output)
        self.assertEqual((self.game / "dlssnr_amd_pass1.dll").read_bytes(), runtime)
        self.assertFalse((self.game / "dlssnr_amd_pass2.dll").exists(), output)
        self.assertEqual((self.game / "nvngx_dlssnr.dll").read_bytes(), nv.read_bytes())
        self.assertFalse((self.game / "dlssnr_on_amd_weights.bin").exists(), output)

    def test_080_older_nvngx_in_game_stays_and_host_folder_gets_the_new_one(self):
        runtime = b"Harmless 0.8.0 runtime fixture; not an executable."
        self.trust_080(runtime)
        (self.package / "version.dll").write_bytes(runtime)
        old_nv = self.nvngx("310.7.0.0", where=self.game)
        old_bytes = old_nv.read_bytes()
        new_nv = self.nvngx("310.8.0.0")
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("Copied nvngx_dlssnr.dll into dlssnr_amd_host", output)
        self.assertEqual((self.game / "nvngx_dlssnr.dll").read_bytes(), old_bytes)
        self.assertEqual((self.game / "dlssnr_amd_host" / "nvngx_dlssnr.dll").read_bytes(), new_nv.read_bytes())

    def test_080_without_weights_or_new_enough_nvngx_fails(self):
        runtime = b"Harmless 0.8.0 runtime fixture; not an executable."
        self.trust_080(runtime)
        (self.package / "version.dll").write_bytes(runtime)
        self.nvngx("310.6.0.0")
        code, output = self.run_direct()
        self.assertEqual(code, 1, output)
        self.assertIn("no nvngx_dlssnr.dll 310.8.0.0 or later to build it from", output)
        self.assertIn("Install FAILED.", output)
        self.assertFalse((self.game / "dlssnr_amd_pass1.dll").exists(), output)

    def test_080_from_installed_neuralhub_with_its_imported_nvngx(self):
        # registry.ini stands in for HKCU (NEURALHUB_STORE); the runtime folder and data\index.ini as
        # the manager writes them.
        runtime = b"Harmless 0.8.0 runtime fixture; not an executable."
        self.trust_080(runtime)
        rt = self.hub_store / "runtime" / "v0.8.0"
        rt.mkdir(parents=True)
        (rt / "neuralhub_host.dll").write_bytes(runtime)
        (self.hub_store / "registry.ini").write_text(
            "[NeuralHub]\nStorePath=" + str(self.hub_store) + "\\\nRuntimePath=" + str(rt) + "\\\n"
            "RuntimeVersion=v0.8.0\nRuntimeAbi=1\n", encoding="utf-8")
        data = self.hub_store / "data"
        nv = self.nvngx("310.8.0.0", name="nr-fixture.dll", where=data)
        (data / "index.ini").write_text("[nr]\nFile=nr-fixture.dll\nFileVersion=310.8.0.0\n", encoding="utf-8")
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("Found danielblnc runtime 0.8.0: " + str(rt / "neuralhub_host.dll"), output)
        self.assertEqual((self.game / "dlssnr_amd_pass1.dll").read_bytes(), runtime)
        self.assertEqual((self.game / "nvngx_dlssnr.dll").read_bytes(), nv.read_bytes())
        self.assertTrue((rt / "neuralhub_host.dll").is_file(), "the installed runtime must stay")

    def test_unsupported_neuralhub_runtime_is_skipped_with_a_note(self):
        self.ready_install()
        rt = self.hub_store / "runtime" / "v0.7.0"
        rt.mkdir(parents=True)
        (rt / "neuralhub_host.dll").write_bytes(b"some other NeuralHub runtime")
        (self.hub_store / "registry.ini").write_text(
            "[NeuralHub]\nRuntimePath=" + str(rt) + "\nRuntimeVersion=v0.7.0\nRuntimeAbi=1\n", encoding="utf-8")
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("NeuralHub's runtime (v0.7.0)", output)
        self.assertEqual((self.game / "dlssnr_amd_pass1.dll").read_bytes(), FAKE_RUNTIME)

    def test_author_dll_may_name_neuralhub_exe(self):
        new = embedded_pe(b"Harmless 0.8.0 runtime fixture; not an executable.")
        self.trust(A080=new)
        hub = self.neuralhub_exe(new, self.root / "elsewhere neuralhub.exe")
        code, output = self.run_direct(flags=("-NonInteractive", "-AuthorDll", str(hub)))
        self.assertEqual(code, 0, output)
        self.assertIn("Took danielblnc runtime 0.8.0 out of", output)
        self.assertEqual((self.game / "dlssnr_amd_pass1.dll").read_bytes(), new)

    def no_seven_zip(self):
        empty = self.root / "no programs"
        empty.mkdir()
        for var in ("PROGRAMFILES", "PROGRAMW6432", "PROGRAMFILES(X86)"):
            self.env[var] = str(empty)
        self.env["PATH"] = os.pathsep.join([str(PS.parent), str(Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32")])

    def test_neuralhub_setup_without_7zip_explains_and_keeps_the_runtime_found(self):
        self.ready_install()
        (self.package / "NeuralHub_v0.8.0_setup.exe").write_bytes(b"MZ fixture NSIS setup, solid LZMA" + bytes(64))
        self.no_seven_zip()
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("needs 7-Zip (7z.exe), which is not installed", output)
        self.assertEqual((self.game / "dlssnr_amd_pass1.dll").read_bytes(), FAKE_RUNTIME)

    def test_neuralhub_setup_is_unpacked_with_7zip(self):
        seven = next((p for p in (Path(os.environ.get(v, "")) / "7-Zip" / "7z.exe"
                                  for v in ("ProgramFiles", "ProgramW6432", "ProgramFiles(x86)")) if p.is_file()), None)
        if not seven:
            self.skipTest("7-Zip not installed")
        new = embedded_pe(b"Harmless 0.8.0 runtime fixture; not an executable.")
        self.trust(A080=new)
        work = self.root / "archive work"
        work.mkdir()
        self.neuralhub_exe(new, work / "neuralhub.exe")
        # 7-Zip opens an archive by its signature, so a .7z named like the NSIS setup stands in for it.
        archive = self.root / "fixture.7z"
        subprocess.run([str(seven), "a", "-bd", str(archive), str(work / "neuralhub.exe")],
                       check=True, stdout=subprocess.DEVNULL, timeout=60)
        archive.replace(self.package / "NeuralHub_v0.8.0_setup.exe")
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("with 7-Zip (the setup is not run)", output)
        self.assertIn("Took danielblnc runtime 0.8.0 out of NeuralHub_v0.8.0_setup.exe (not run)", output)
        self.assertEqual((self.game / "dlssnr_amd_pass1.dll").read_bytes(), new)

    def test_neuralhub_stub_in_game_is_moved_aside(self):
        self.ready_install()
        (self.game / "winmm.dll").write_bytes(b"MZ fixture stub NeuralHubStub_Abi NeuralHub_HostStart")
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("winmm.dll is NeuralHub's stub", output)
        self.assertFalse((self.game / "winmm.dll").exists(), output)
        self.assertTrue(list(self.game.glob("backup-amd-presr-*/winmm.dll.moved")), output)

    def test_noninteractive_install_failures_do_not_wait(self):
        code, output = self.run_direct(game=self.root / "missing")
        self.assertEqual(code, 1, output)
        self.assertIn("Install FAILED.", output)
        self.broken_author_setup()
        code, output = self.run_direct()
        self.assertEqual(code, 1, output)
        self.assertIn("Unexpected install error:", output)

    def test_uninstall_copied_into_game_folder_runs_in_place(self):
        """Setup copies Uninstall_OptiScaler_NR into the game folder.

        Double-click there has no folder picker: the script uses its own
        directory. Cancel must keep files; Y deletes the planned list.
        """
        self.ready_install()
        code, output = self.run_batch()
        self.assertEqual(code, 0, output)
        bat = self.game / "Uninstall_OptiScaler_NR.bat"
        script = self.game / "Uninstall_OptiScaler_NR.ps1"
        self.assertTrue(bat.is_file(), output)
        self.assertTrue(script.is_file(), output)
        pass1 = self.game / "dlssnr_amd_pass1.dll"
        self.assertTrue(pass1.is_file(), output)

        def run_in_place(stdin):
            args = [str(PS), "-NoProfile", "-ExecutionPolicy", "Bypass", "-STA",
                    "-File", str(script), "-NoPause"]
            return self.run_process(args, stdin)

        # Setup always creates backup-amd-presr-*, so keep-backups is asked first.
        code, output = run_in_place("Y\nN\n")
        self.assertEqual(code, 0, output)
        self.assertIn("Planned deletions", output)
        self.assertIn("Cancelled.", output)
        self.assertNotIn("Uninstall SUCCEEDED.", output)
        self.assertTrue(pass1.is_file(), output)
        self.assertTrue(script.is_file(), output)

        code, output = run_in_place("Y\nY\n")
        self.assertEqual(code, 0, output)
        self.assertIn("Planned deletions", output)
        self.assertIn("Uninstall SUCCEEDED.", output)
        self.assertFalse(pass1.exists(), output)
        self.assertFalse(script.exists(), output)
        self.assertFalse(bat.exists(), output)

    def test_uninstall_asks_keep_backups_before_planned_list(self):
        self.ready_install()
        code, output = self.run_batch()
        self.assertEqual(code, 0, output)
        script = self.game / "Uninstall_OptiScaler_NR.ps1"
        backup = self.game / "backup-amd-presr-fixture"
        backup.mkdir()
        (backup / "old.dll").write_bytes(b"old")
        pass1 = self.game / "dlssnr_amd_pass1.dll"

        def run_in_place(stdin):
            args = [str(PS), "-NoProfile", "-ExecutionPolicy", "Bypass", "-STA",
                    "-File", str(script), "-NoPause"]
            return self.run_process(args, stdin)

        code, output = run_in_place("Y\nN\n")
        self.assertEqual(code, 0, output)
        keep_at = output.lower().find("old backup folder")
        planned_at = output.lower().find("planned deletions")
        self.assertNotEqual(keep_at, -1, output)
        self.assertNotEqual(planned_at, -1, output)
        self.assertLess(keep_at, planned_at, output)
        self.assertIn("Cancelled.", output)
        self.assertTrue(backup.is_dir(), output)
        self.assertTrue(pass1.is_file(), output)

        code, output = run_in_place("n\ny\n")
        self.assertEqual(code, 0, output)
        self.assertIn("Uninstall SUCCEEDED.", output)
        self.assertFalse(backup.exists(), output)
        self.assertFalse(pass1.exists(), output)

    def test_uninstall_success_and_cancel_pause_once(self):
        for answer, message in (
            ("Y\n", "Uninstall SUCCEEDED."),
            ("yes\n", "Uninstall SUCCEEDED."),
            ("N\n", "Cancelled."),
            ("NO\n", "Cancelled."),
        ):
            with self.subTest(answer=answer.strip()):
                code, output = self.run_batch(name="Uninstall", stdin=answer)
                self.assertEqual(code, 0, output)
                self.assertIn(message, output)
                self.assertNotIn("Uninstall failed", output)
                if answer.startswith("N"):
                    self.assertNotIn("Uninstall SUCCEEDED.", output)

    def test_uninstall_explicit_failure_pauses_once(self):
        code, output = self.run_batch(name="Uninstall", game=self.root / "missing")
        self.assertEqual(code, 1, output)
        self.assertIn("Uninstall FAILED.", output)
        self.assertIn("Uninstall failed (exit code 1)", output)

    def test_uninstall_unhandled_exception_uses_trap(self):
        _, script_name = self.launcher_files("Uninstall")
        script = self.package / script_name
        source = script.read_text(encoding="utf-8-sig")
        insertion = "$game = (Resolve-Path -LiteralPath $GameDir).Path"
        self.assertEqual(source.count(insertion), 1)
        write_ps(script, source.replace(insertion, insertion + "\nthrow 'fixture unexpected error'"))
        code, output = self.run_batch(name="Uninstall")
        self.assertEqual(code, 1, output)
        self.assertIn("fixture unexpected error", output)
        self.assertIn("Uninstall FAILED.", output)

    def test_uninstall_parse_failure_has_batch_fallback(self):
        _, script_name = self.launcher_files("Uninstall")
        write_ps(self.package / script_name, "param(\n")
        code, output = self.run_batch(name="Uninstall")
        self.assertNotEqual(code, 0, output)
        self.assertIn(f"Uninstall failed (exit code {code})", output)

    def test_noninteractive_uninstall_does_not_wait(self):
        for game, expected in ((self.game, 0), (self.root / "missing", 1)):
            with self.subTest(expected=expected):
                code, output = self.run_direct(name="Uninstall", game=game)
                self.assertEqual(code, expected, output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
