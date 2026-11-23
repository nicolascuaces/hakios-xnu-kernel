#!/usr/bin/env python3
"""
A22: HakiOS's saved Wi-Fi networks (fw-wifi/network, which userland/build.sh copies to root/fw/network).
The driver (redwood_a22wifi.c) joins the FIRST one in the list that it hears at boot; boot-arg wifi_net=N forces number N.
Passwords are never shown and never end up in the shell history.

  python3 tools/a22-redes.py ~/dev/hakios list          -> the networks, numbered (the N of wifi_net=N)
  python3 tools/a22-redes.py ~/dev/hakios add           -> asks for a name and a password (empty = open network)
  python3 tools/a22-redes.py ~/dev/hakios remove N      -> deletes network N
  python3 tools/a22-redes.py ~/dev/hakios first N       -> puts network N first (the preferred one)
  python3 tools/a22-redes.py ~/dev/hakios install       -> copies redwood_a22wifi.c (next to this script) into XNU

(The old Spanish commands still work: lista, agregar, quitar, primero, instalar, forzar.)

File format (the usual one, repeated): name, password, name, password... At most 8 networks.
"""
import getpass, hashlib, os, shutil, string, sys, tempfile

XNU = "xnu-src/xnu-8796.121.2"
ARCHIVO = "fw-wifi/network"
DRIVER = "redwood_a22wifi.c"
DESTINO = XNU + "/bsd/dev/redwood_a22wifi.c"
MAXIMO = 8
# sha256 of the previous redwood_a22wifi.c (step 13, before the list): only that one is replaced without asking
ANTERIOR = "1d2591eea16c9029e06587fd9c6d47426079542e7aaface9a33a32b1f835b874"

# English command -> the old Spanish one it replaces
ALIASES = {"lista": "list", "agregar": "add", "quitar": "remove", "primero": "first", "instalar": "install"}


def leer(ruta):
    """Reads the file the way the driver does: skips empty lines where a name goes, strips the \\r."""
    if not os.path.isfile(ruta):
        return []
    lineas = open(ruta, "rb").read().split(b"\n")
    redes, i = [], 0
    while i < len(lineas):
        nombre = lineas[i].rstrip(b"\r"); i += 1
        if not nombre:
            continue
        clave = lineas[i].rstrip(b"\r") if i < len(lineas) else b""; i += 1
        redes.append((nombre, clave))
    return redes


def escribir(ruta, redes):
    os.makedirs(os.path.dirname(ruta), exist_ok=True)
    datos = b"".join(n + b"\n" + c + b"\n" for n, c in redes)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(ruta), prefix=".network.")
    try:
        os.fchmod(fd, 0o600)
        os.write(fd, datos)
        os.close(fd)
        os.replace(tmp, ruta)
    except BaseException:
        os.unlink(tmp)
        raise
    os.chmod(ruta, 0o600)


def mostrar(redes):
    if not redes:
        print("There are no saved networks (the driver joins nothing at boot; pick one in Settings).")
        return
    for n, (nombre, clave) in enumerate(redes, 1):
        tipo = "open" if not clave else "WPA2, with password"
        print("  %d. %-32s %s%s" % (n, nombre.decode("utf-8", "replace"), tipo, "  <- preferred" if n == 1 else ""))
    print("\nAt boot it joins the first one it hears. To force one: wifi_net=N in XNU_ARGS.")


def clave_valida(c):
    if c == "":
        return None
    if any(ch not in string.printable or ch in "\r\n\t\x0b\x0c" for ch in c):
        return "printable ASCII characters only"
    if len(c) == 64 and all(ch in string.hexdigits for ch in c):
        return None
    if not 8 <= len(c) <= 63:
        return "WPA2 needs 8 to 63 characters (it has %d)" % len(c)
    return None


def agregar(ruta, redes):
    if len(redes) >= MAXIMO:
        raise SystemExit("There are already %d networks (the maximum). Remove one with: remove N" % MAXIMO)
    nombre = input("Network name (exactly as is, with capitals and spaces): ")
    nb = nombre.encode("utf-8")
    if not 1 <= len(nb) <= 32 or "\n" in nombre or "\r" in nombre:
        raise SystemExit("The name must be 1 to 32 bytes long.")
    clave = getpass.getpass("Password (not shown; empty Enter = open network): ")
    error = clave_valida(clave)
    if error:
        raise SystemExit("Invalid password: %s. Nothing was saved." % error)
    if clave:
        if getpass.getpass("Repeat the password: ") != clave:
            raise SystemExit("They do not match. Nothing was saved.")
    elif input("OPEN network (no password). Sure? [y/N] ").strip().lower() not in ("y", "s"):
        raise SystemExit("Nothing was saved.")
    cb = clave.encode("ascii")
    for i, (n, _) in enumerate(redes):
        if n == nb:
            redes[i] = (nb, cb)
            escribir(ruta, redes)
            print("Updated the password of network %d." % (i + 1))
            return redes
    redes.append((nb, cb))
    escribir(ruta, redes)
    print("Added as network %d." % len(redes))
    return redes


def numero(redes, arg):
    try:
        n = int(arg)
    except ValueError:
        n = 0
    if not 1 <= n <= len(redes):
        raise SystemExit("N must go from 1 to %d (see: list)." % len(redes))
    return n - 1


def instalar(raiz):
    aqui = os.path.dirname(os.path.abspath(__file__))
    origen, destino = os.path.join(aqui, DRIVER), os.path.join(raiz, DESTINO)
    if not os.path.isfile(origen):
        raise SystemExit("%s is missing next to this script." % DRIVER)
    if "wf_saved_read" not in open(origen, encoding="utf-8").read():
        raise SystemExit("The %s next to this script is the old one (it has no list). Download it again." % DRIVER)
    if not os.path.isfile(destino):
        raise SystemExit("%s does not exist: apply tools/a22-wifi.py first." % DESTINO)
    actual = hashlib.sha256(open(destino, "rb").read()).hexdigest()
    nuevo = hashlib.sha256(open(origen, "rb").read()).hexdigest()
    if actual == nuevo:
        print("was there  |", DESTINO); return
    if actual != ANTERIOR and not ({"force", "forzar"} & set(sys.argv[3:])):
        raise SystemExit("Your %s is not the one I expect (did you change it afterwards?). Touching nothing.\n"
                         "Send me that file, or use: install force (saves a .redes.orig copy)." % DESTINO)
    if not os.path.exists(destino + ".redes.orig"):
        shutil.copy2(destino, destino + ".redes.orig")
    shutil.copy2(origen, destino)
    print("copied     |", DESTINO, "(copy of the previous one in .redes.orig)")
    print("\nNow the kernel:  nice -n 10 ./xnu-build.sh -j2 -k")


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    raiz = os.path.expanduser(sys.argv[1])
    orden = ALIASES.get(sys.argv[2], sys.argv[2])
    if not os.path.isdir(os.path.join(raiz, "userland")):
        raise SystemExit("%s does not look like the hakios tree (userland/ is missing)." % raiz)
    if orden == "install":
        return instalar(raiz)
    ruta = os.path.join(raiz, ARCHIVO)
    redes = leer(ruta)
    if orden == "list":
        pass
    elif orden == "add":
        redes = agregar(ruta, redes)
    elif orden in ("remove", "first"):
        if len(sys.argv) < 4:
            raise SystemExit("N is missing: %s N" % orden)
        i = numero(redes, sys.argv[3])
        red = redes.pop(i)
        if orden == "first":
            redes.insert(0, red)
        escribir(ruta, redes)
        print("Done.")
    else:
        raise SystemExit(__doc__)
    print()
    mostrar(redes)
    if orden != "list":
        print("\nFor the phone to see the change: userland/build.sh (your usual recipe) and ./xl_x6826b build.")


if __name__ == "__main__":
    main()
