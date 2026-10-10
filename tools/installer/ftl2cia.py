"""FTL2CIA: makes an installable ftl3ds.cia from your own copy of FTL.

Pick your ftl.dat (or the FTL folder) and where to save the CIA, then press Build. The CIA carries
your ftl.dat inside, so nothing else needs copying to the SD card; install it with FBI.

Runs as a packaged .exe (tools/installer/build_installer.sh) or as a script:
    python ftl2cia.py
The engine files it needs (ftl3ds.elf, ftl3ds.rsf, banner.bnr, icon.png, makerom, bannertool) sit in
the "payload" folder next to it, or inside the packaged .exe.
"""
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

APP = "FTL2CIA"


def payload_dir():
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, "payload")


def find_dat(path):
    """ftl.dat itself, or the FTL folder holding it (Steam, GOG and Humble keep it at the top)."""
    if os.path.isdir(path):
        for name in ("ftl.dat", "FTL.dat"):
            p = os.path.join(path, name)
            if os.path.isfile(p):
                return p
        # macOS app bundle
        p = os.path.join(path, "Contents", "Resources", "ftl.dat")
        return p if os.path.isfile(p) else None
    return path if os.path.isfile(path) else None


def check_dat(path):
    """An FTL 1.6+ data file starts with "PKG\\n"; older versions used data.dat/resource.dat."""
    with open(path, "rb") as f:
        return f.read(4) == b"PKG\n"


def ftl_icon(exe_path, out_png):
    """FTL's own 48x48 icon from FTLGame.exe, if it's there (else the builder's own is used)."""
    try:
        import pefile
        from PIL import Image
    except ImportError:
        return False
    try:
        pe = pefile.PE(exe_path)
        best = None
        for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
            if t.id != 3:  # RT_ICON
                continue
            for e in t.directory.entries:
                for lang in e.directory.entries:
                    d = pe.get_data(lang.data.struct.OffsetToData, lang.data.struct.Size)
                    if d[:8] == b"\x89PNG\r\n\x1a\n":
                        im = Image.open(io.BytesIO(d))
                        w, bpp = im.width, 32
                    else:
                        w, = struct.unpack("<i", d[4:8])
                        bpp, = struct.unpack("<H", d[14:16])
                        ico = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", w % 256, w % 256, 0, 0, 1, bpp,
                                                                         len(d), 22) + d
                        im = Image.open(io.BytesIO(ico))
                    if bpp == 32 and (best is None or w > best.width):
                        best = im.convert("RGBA")
        if best is None:
            return False
        best.resize((48, 48), Image.LANCZOS).save(out_png)
        return True
    except Exception:
        return False


def run(cmd, log):
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    p = subprocess.run(cmd, capture_output=True, text=True, creationflags=flags)
    for line in (p.stdout + p.stderr).splitlines():
        if line.strip():
            log(line)
    if p.returncode != 0:
        raise RuntimeError(f"{os.path.basename(cmd[0])} failed (code {p.returncode})")


def build_cia(dat_path, out_path, log):
    pay = payload_dir()
    exe = ".exe" if os.name == "nt" else ""
    makerom = os.path.join(pay, "makerom" + exe)
    bannertool = os.path.join(pay, "bannertool" + exe)
    for f in ("ftl3ds.elf", "ftl3ds.rsf", "banner.bnr", "icon.png"):
        if not os.path.isfile(os.path.join(pay, f)):
            raise RuntimeError(f"The builder is missing {f}: download it again.")
    with tempfile.TemporaryDirectory(prefix="ftl3ds_") as work:
        romfs = os.path.join(work, "romfs")
        os.makedirs(romfs)
        log("Copying ftl.dat...")
        shutil.copyfile(dat_path, os.path.join(romfs, "ftl.dat"))
        icon = os.path.join(work, "icon.png")
        game_exe = os.path.join(os.path.dirname(dat_path), "FTLGame.exe")
        if os.path.isfile(game_exe) and ftl_icon(game_exe, icon):
            log("Using FTL's icon from FTLGame.exe.")
        else:
            shutil.copyfile(os.path.join(pay, "icon.png"), icon)
        smdh = os.path.join(work, "icon.smdh")
        log("Making the home menu icon...")
        run([bannertool, "makesmdh", "-s", "FTL3DS", "-l", "FTL: Faster Than Light (FTL3DS engine)",
             "-p", "FTL3DS contributors", "-f", "visible,allow3d,recordusage", "-i", icon, "-o", smdh], log)
        log("Building the CIA (this takes a minute)...")
        run([makerom, "-f", "cia", "-o", out_path, "-elf", os.path.join(pay, "ftl3ds.elf"),
             "-rsf", os.path.join(pay, "ftl3ds.rsf"), "-icon", smdh, "-banner", os.path.join(pay, "banner.bnr"),
             "-target", "t", f"-DAPP_ROMFS={romfs}", "-major", "0", "-minor", "1", "-micro", "0"], log)
    if not os.path.isfile(out_path):
        raise RuntimeError("makerom didn't write the CIA.")


class App:
    def __init__(self, root):
        self.root = root
        root.title(APP)
        root.resizable(False, False)
        f = ttk.Frame(root, padding=14)
        f.grid()
        ttk.Label(f, text="Makes ftl3ds.cia from your own copy of FTL: Faster Than Light (1.6 or later).",
                  wraplength=460).grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 10))

        ttk.Label(f, text="1. Your ftl.dat (or FTL's folder):").grid(row=1, column=0, columnspan=3, sticky="w")
        self.dat = tk.StringVar(value=self.guess_dat())
        ttk.Entry(f, textvariable=self.dat, width=58).grid(row=2, column=0, columnspan=2, sticky="we")
        ttk.Button(f, text="Browse...", command=self.pick_dat).grid(row=2, column=2, padx=(6, 0))

        ttk.Label(f, text="2. Save the CIA as:").grid(row=3, column=0, columnspan=3, sticky="w", pady=(10, 0))
        self.out = tk.StringVar(value=os.path.join(os.path.expanduser("~"), "Desktop", "ftl3ds.cia"))
        ttk.Entry(f, textvariable=self.out, width=58).grid(row=4, column=0, columnspan=2, sticky="we")
        ttk.Button(f, text="Browse...", command=self.pick_out).grid(row=4, column=2, padx=(6, 0))

        self.go = ttk.Button(f, text="Build CIA", command=self.start)
        self.go.grid(row=5, column=0, columnspan=3, pady=12)
        self.bar = ttk.Progressbar(f, mode="indeterminate", length=460)
        self.bar.grid(row=6, column=0, columnspan=3)
        self.log_box = tk.Text(f, width=64, height=9, state="disabled", font=("Consolas", 9))
        self.log_box.grid(row=7, column=0, columnspan=3, pady=(10, 0))
        ttk.Label(f, text="Install the CIA with FBI. The CIA contains your copy of FTL's data: don't share it.",
                  wraplength=460, foreground="#a33").grid(row=8, column=0, columnspan=3, sticky="w", pady=(8, 0))

    @staticmethod
    def guess_dat():
        """FTL in any Steam library (Steam lists them in libraryfolders.vdf), or GOG's usual places."""
        libraries = []
        for steam in (r"C:\Program Files (x86)\Steam", r"C:\Program Files\Steam"):
            vdf = os.path.join(steam, "steamapps", "libraryfolders.vdf")
            libraries.append(steam)
            try:
                import re
                with open(vdf, encoding="utf-8", errors="replace") as f:
                    libraries += [p.replace("\\\\", "\\") for p in re.findall(r'"path"\s+"([^"]+)"', f.read())]
            except OSError:
                pass
        candidates = [os.path.join(lib, "steamapps", "common", "FTL Faster Than Light") for lib in libraries]
        candidates += [r"C:\GOG Games\FTL Advanced Edition", r"C:\Program Files (x86)\GOG Galaxy\Games\FTL Advanced Edition"]
        for p in candidates:
            d = find_dat(p)
            if d:
                return d
        return ""

    def pick_dat(self):
        p = filedialog.askopenfilename(title="Find ftl.dat in your FTL folder",
                                       filetypes=[("FTL data", "ftl.dat"), ("All files", "*.*")])
        if p:
            self.dat.set(p)

    def pick_out(self):
        p = filedialog.asksaveasfilename(title="Save the CIA as", defaultextension=".cia", initialfile="ftl3ds.cia",
                                         filetypes=[("3DS CIA", "*.cia")])
        if p:
            self.out.set(p)

    def log(self, text):
        def add():
            self.log_box.configure(state="normal")
            self.log_box.insert("end", text + "\n")
            self.log_box.see("end")
            self.log_box.configure(state="disabled")
        self.root.after(0, add)

    def start(self):
        dat = find_dat(self.dat.get().strip().strip('"'))
        out = self.out.get().strip().strip('"')
        if not dat:
            messagebox.showerror(APP, "Couldn't find ftl.dat there. Pick the ftl.dat file in your FTL folder.")
            return
        if not check_dat(dat):
            messagebox.showerror(APP, "That isn't an FTL 1.6+ ftl.dat. Older FTL versions (data.dat) aren't "
                                      "supported: update FTL, or check the file isn't modded.")
            return
        if not out:
            messagebox.showerror(APP, "Choose where to save the CIA.")
            return
        if not out.lower().endswith(".cia"):
            out += ".cia"
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        self.go.configure(state="disabled")
        self.bar.start(12)

        def work():
            try:
                build_cia(dat, out, self.log)
                self.log("Done: " + out)
                self.root.after(0, lambda: messagebox.showinfo(APP, f"Made {out}\n\nInstall it on your 3DS with FBI."))
            except Exception as e:
                self.log("Error: " + str(e))
                err = str(e)
                self.root.after(0, lambda: messagebox.showerror(APP, err))
            finally:
                self.root.after(0, lambda: (self.bar.stop(), self.go.configure(state="normal")))
        threading.Thread(target=work, daemon=True).start()


if __name__ == "__main__":
    root = tk.Tk()
    App(root)
    root.mainloop()
