# Fichiers Vigibot RDK X5

Scripts déployés vers `/usr/local/vigiclient/` par `install/install.sh`.

| Fichier | Rôle |
|---------|------|
| `vigi-encode-rdk.py` / `.sh` | Source vidéo 0 (caméra brute, libx264) |
| `vigi-encode-yolo.py` / `.sh` | Source vidéo 1 (overlay YOLO BPU) |
| `rdk-pigpio.js` | API pigpio-like → `rdk-gpio-helper.py` |
| `rdk-gpio-helper.py` | Daemon GPIO (BCM→BOARD, soft PWM) |
| `rdk-i2c-bus.js` / `rdk-pca9685.js` | Stubs (pas de module I2C sur le robot de test) |

## `clientrobotpi.js`

Le client Node Vigibot principal **n'est pas redistribué** dans ce dépôt (binaire propriétaire Vigibot). Il doit déjà être installé par Vigibot dans `/usr/local/vigiclient/`.

Pour rapatrier une version patchée depuis votre carte de référence :

```bash
./scripts/fetch-from-board.sh
```

Cela copie aussi `clientrobotpi.js` (patches latence, etc.) si présent sur la carte.

**Ne jamais committer** `robot.json` avec identifiants réels.
