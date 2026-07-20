# Fichiers Vigibot RDK X5

Scripts déployés vers `/usr/local/vigiclient/` par `install/install.sh`.

| Fichier | Rôle |
|---------|------|
| `vigi-encode-rdk.py` / `.sh` | Source vidéo 0 (caméra brute, libx264) |
| `vigi-encode-yolo.py` / `.sh` | Source vidéo 1 (overlay YOLO BPU) |
| `rdk-pigpio.js` | API pigpio-like → helper natif, fallback Python |
| `rdk-gpio-helper.c` | Source du daemon WiringPi C (BCM→BOARD, soft PWM temps réel) |
| `rdk-gpio-helper` | Binaire compilé sur la RDK par l'installateur |
| `rdk-gpio-helper.py` | Ancien backend Hobot.GPIO, conservé comme fallback |
| `rdk-i2c-bus.js` / `rdk-pca9685.js` | Stubs (pas de module I2C sur le robot de test) |

## `clientrobotpi.js`

Le client Node Vigibot principal **n'est pas redistribué** dans ce dépôt (binaire propriétaire Vigibot). Il doit déjà être installé par Vigibot dans `/usr/local/vigiclient/`.

Pour rapatrier une version patchée depuis votre carte de référence :

```bash
./scripts/fetch-from-board.sh
```

Cela copie aussi `clientrobotpi.js` (patches latence, etc.) si présent sur la carte.

**Ne jamais committer** `robot.json` avec identifiants réels.
