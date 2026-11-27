"""
UNO Q - cote Linux : passerelle Minitel <-> MiniPavi (TCP go.minipavi.fr:516).
Le microcontroleur ne fait que transporter les octets (Bridge) ; ici on gere
l'ecran d'accueil, la touche CONNEXION/FIN et la connexion au serveur.
"""
import socket
import threading
import time

from arduino.app_utils import App, Bridge

HOST, PORT = "go.minipavi.fr", 516
OCTETS_PAR_S = 118              # 1200 bauds 7E1 = 120 octets/s

# Etats affiches sur la matrice
BOOT, READY, CONNECTING, ONLINE, ERROR = range(5)

ESC, PRO3 = 0x1B, 0x3B
AIG_OFF, AIG_ON = 0x60, 0x61
RX_ECRAN, RX_MODEM, RX_PRISE = 0x58, 0x5A, 0x5B
TX_CLAVIER, TX_PRISE = 0x51, 0x53
SEP, KEY_CNX_FIN = 0x13, 0x49

lock = threading.Lock()
tx_lock = threading.Lock()
connecting = threading.Event()
sock = None
prev_sep = False


def log(*a):
    print("[minitel]", *a, flush=True)


def matrix(state):
    try:
        Bridge.call("set_state", state)
    except Exception as e:
        log("matrice:", e)


def to_minitel(data: bytes):
    """Envoie au Minitel en respectant son debit."""
    with tx_lock:                     # un seul emetteur a la fois
        i = 0
        while i < len(data):
            chunk = data[i:i + 64]
            try:
                n = int(Bridge.call("minitel_write", chunk.hex(), timeout=10) or 0)
            except Exception as e:
                log("envoi minitel :", e)
                time.sleep(1)
                n = 0
            i += n
            # La file du MCU se vide a ~120 octets/s : on suit ce rythme.
            time.sleep(max(n, 8) / OCTETS_PAR_S)


# ---- Ecran Videotex ----------------------------------------------------
def at(r, c):
    return bytes([0x1F, 0x40 + r, 0x40 + c])


def configure_minitel():
    seq = b""
    for on, rx, tx in [
        (False, RX_MODEM, TX_CLAVIER),
        (False, RX_ECRAN, TX_CLAVIER),   # le serveur fait l'echo
        (True, RX_PRISE, TX_CLAVIER),
        (False, RX_MODEM, TX_PRISE),
        (True, RX_ECRAN, TX_PRISE),
    ]:
        seq += bytes([ESC, PRO3, AIG_ON if on else AIG_OFF, rx, tx])
    to_minitel(seq)


def home_screen(status: str):
    s = b"\x0c\x14"                                   # efface, curseur off
    s += at(8, 10) + b"\x1bM" + b"M I N I P A V I"     # double hauteur
    s += at(12, 5) + b"Passerelle Wi-Fi  UNO Q <> Minitel"
    s += at(16, 6) + status.encode("ascii", "replace")
    s += at(20, 6) + b"Appuyez sur  CONNEXION/FIN"
    to_minitel(s)


# ---- Connexion serveur -------------------------------------------------
def server_reader(s):
    global sock
    try:
        while True:
            data = s.recv(256)
            if not data:
                break
            to_minitel(data)
    except OSError as e:
        log("lecture serveur:", e)
    with lock:
        if sock is s:
            sock = None
    try:
        s.close()
    except OSError:
        pass
    log("deconnecte")
    matrix(READY)
    home_screen("Deconnecte.")


def connect():
    if connecting.is_set():
        log("connexion deja en cours, appui ignore")
        return
    connecting.set()
    try:
        _connect()
    except Exception as e:
        log("echec connexion:", e)
        matrix(ERROR)
    finally:
        connecting.clear()


def _connect():
    global sock
    log("connexion a", HOST)
    matrix(CONNECTING)
    home_screen("Connexion en cours...")
    try:
        s = socket.create_connection((HOST, PORT), timeout=10)
        s.settimeout(None)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except OSError as e:
        log("connexion impossible:", e)
        matrix(ERROR)
        home_screen("Serveur injoignable.")
        return
    configure_minitel()
    to_minitel(b"\x0c\x11")                            # efface, curseur on
    with lock:
        sock = s
    matrix(ONLINE)
    log("connecte a", HOST)
    threading.Thread(target=server_reader, args=(s,), daemon=True).start()


# ---- Octets venant du Minitel -----------------------------------------
def from_minitel(hexstr: str):
    global prev_sep
    # Le Minitel 1 de 1985 code CONNEXION/FIN en SEP 0x59 (0x49 sur les modeles recents)
    data = bytes.fromhex(hexstr).replace(b"\x13\x59", b"\x13\x49")
    with lock:
        s = sock
    if s is not None:
        try:
            s.sendall(data)
        except OSError as e:
            log("envoi serveur:", e)
        return
    for b in data:
        log("clavier 0x%02X" % b)
        if prev_sep and b == KEY_CNX_FIN:
            prev_sep = False
            threading.Thread(target=connect, daemon=True).start()
            return
        prev_sep = (b == SEP)


Bridge.provide("from_minitel", from_minitel)


def start():
    # Attend que le sketch du microcontroleur ait publie ses fonctions
    for i in range(60):
        try:
            Bridge.call("set_state", BOOT)
            log("microcontroleur pret apres %ds" % i)
            break
        except Exception as e:
            if i % 10 == 0:
                log("attente du microcontroleur:", e)
            time.sleep(1)
    else:
        log("microcontroleur injoignable, abandon")
        return
    configure_minitel()
    home_screen("Pret.")
    matrix(READY)
    log("pret")


threading.Thread(target=start, daemon=True).start()
App.run()
