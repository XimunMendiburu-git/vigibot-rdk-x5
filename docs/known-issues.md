# Problèmes connus et runbook

Guide opérationnel pour le robot RDK X5 sous Vigibot. Complète le [rapport POC](./poc-vigibot-rdk-x5.md).

---

## 1. Vidéo noire après changement de source (0 ↔ 1)

### Symptômes

- Écran noir sur Vigibot
- Logs : `Mipi csi0 has been used`, `camera.open_cam failed`
- Ancien encodeur parfois encore actif (`sent … nv12 frames` dans le log)

### Cause

La CSI n'est pas libérée avant le lancement du nouvel encodeur.

### Procédure

```bash
kill -9 $(pgrep -f vigi-encode) 2>/dev/null
sleep 2
systemctl restart vigiclient
sleep 5
grep -E 'open_cam|connected|sent |camera ready' /var/log/vigiclient.log | tail -20
```

**Attention** : éviter `pkill -f vigi-encode` sans précision — peut matcher la ligne de commande du shell et produire des `Killed` inattendus. Préférer `pgrep -f 'vigi-encode-yolo|vigi-encode-rdk'`.

### Prévention

- Attendre 2–3 s entre deux switches de source sur Vigibot
- Ne pas sauver la config hardware pendant que la vidéo tourne sans laisser le temps au restart

---

## 2. Fausse alarme de latence gigantesque

### Symptômes

```
1784300838532 ms latency, stopping of motors and streams
```

Puis retour à ~300 ms. Failsafe moteurs déclenché.

### Cause

Dans `clientrobotpi.js` (~ligne 668) :

```javascript
lastTimestamp = data.boucleVideoCommande;
```

Quand le serveur envoie `boucleVideoCommande = 0`, le calcul devient `Date.now() - 0 ≈ 1.7e12 ms`.

### Contournements appliqués

1. Garde : n'assigner que si `boucleVideoCommande > 1e12`
2. Neutralisation condition `LATENCYALARMBEGIN` (`false &&`)
3. Seuils relevés : END 60000 / BEGIN 120000 ms
4. Clear vidéo alarme déjà commenté

### Conséquence

Protection latence réelle **affaiblie ou désactivée**. À ré-implémenter proprement (timestamp initialisé, validation plage, failsafe graduel).

---

## 3. Robot absent sur Vigibot après reboot

### Symptômes

SSH OK mais robot invisible sur le site.

### Causes fréquentes

- Service `vigiclient` non enabled au boot
- Changement réseau Wi-Fi (déconnexion temporaire)
- Client pas reconnecté au serveur

### Procédure

```bash
systemctl is-enabled vigiclient    # doit afficher: enabled
systemctl status vigiclient
grep 'Connected to https://www.vigibot.com' /var/log/vigiclient.log | tail -5
ping -c 2 www.vigibot.com
```

Activer au boot si nécessaire :

```bash
systemctl enable vigiclient
systemctl start vigiclient
```

---

## 4. Moteurs qui tournent au neutre (stick centré)

### Symptômes

Roues avancent légèrement au repos ; s'arrêtent en commandant un léger recul.

### Causes

- Deadzone Vigibot trop étroite (`INS: [-100, -1, 1, 100]`)
- Offset / trim des sticks télécommande
- `pwmWrite` stub (avant implémentation bridge)

### Solution

Dans la config hardware Vigibot, par roue `PwmPwm` :

```json
"INS": [-100, -15, 15, 100],
"OUTS": [-100, 0, 0, 100]
```

Vérifier trim sticks à 0 sur la télécommande Vigibot.

---

## 5. Servos qui tremblent au repos

### Symptômes

Maintien instable, vibration audible/visible au repos.

### Cause

Ancien backend Python : soft PWM userspace non temps-réel.

### Options

| Option | Action |
|--------|--------|
| Accepter | Vivre avec pour téléop basique |
| Couper au repos | Pulse 0 (Floating) — plus de tremblement, pas de couple de maintien |
| **Actuel** | Helper WiringPi C avec threads `SCHED_FIFO`, validation en cours |
| Long terme | Ajouter module PCA9685 (I2C) |

Ne pas activer PWM0/PWM1 avec un overlay personnalisé : ce test a désactivé le Wi-Fi du robot de référence. Voir [gpio-mapping.md](./gpio-mapping.md).

---

## 6. Déploiement scripts via SSH

### Problème : collage cassé

Symptôme : `[200~`, `^[[201~`, heredoc tronqué, fichier Python incomplet.

### Solutions

```bash
# Désactiver bracketed paste avant gros collage
bind 'set enable-bracketed-paste off'
```

Préférer transfert depuis PC :

```powershell
scp vigi-encode-yolo.py sunrise@10.146.245.115:/tmp/
```

Puis sur la carte :

```bash
cp /tmp/vigi-encode-yolo.py /usr/local/vigiclient/
python3 -m py_compile /usr/local/vigiclient/vigi-encode-yolo.py && echo OK
wc -l /usr/local/vigiclient/vigi-encode-yolo.py
```

---

## 7. Commandes diagnostic rapides

### Service et processus

```bash
systemctl status vigiclient --no-pager
pgrep -af 'clientrobotpi|vigi-encode|rdk-gpio'
```

### Logs

```bash
tail -n 50 /var/log/vigiclient.log
sudo journalctl -u vigiclient -n 80 --no-pager
sudo journalctl -u vigiclient -f | grep --line-buffered 'VIDEO NAL'
```

Note : les logs encodeur Python vont souvent sur stderr → visibles dans journald, pas toujours dans `vigiclient.log`.

### Vidéo TCP

```bash
sudo ss -tpn | grep 8043
```

Attendu : Node en LISTEN + connexion depuis le process encodeur.

### GPIO helper

```bash
pgrep -af rdk-gpio-helper
grep -n 'servoWrite\|pwmWrite' /usr/local/vigiclient/rdk-pigpio.js
```

### I2C (si PCA ajouté plus tard)

```bash
ls -l /dev/i2c-*
for b in 0 2 3 4 5 6 7 8; do echo "=== bus $b ==="; i2cdetect -y -r $b 2>/dev/null | grep -E '40|70|UU'; done
```

---

## 8. Issues ouverts (post-POC)

| Issue | Priorité | Piste |
|-------|----------|-------|
| Encodeur HW incompatible navigateur | Moyenne | Analyse slices NAL, WebCodecs |
| Servos tremblent au repos | Haute | PWM HW X5 ou PCA9685 |
| Overlay PWM0/PWM1 coupe le Wi-Fi | Critique | Ne pas déployer ; analyser le pinmux hors production |
| Switch source CSI fragile | Moyenne | Cleanup garanti, watchdog encodeur |
| Failsafe latence neutralisé | Haute | Ré-implémenter garde timestamp |
| Stubs I2C/PCA | Basse | Si hardware ajouté |
| Scripts non versionnés | Moyenne | Dépôt dédié + CI déploiement |
| IR / switches non validés | Basse | Tests bouton Vigibot |
| Warning HBRT vs modèle YOLO | Basse | Aligner OpenExplorer |

---

## 9. Contacts et chemins utiles

| Ressource | Chemin / URL |
|-----------|--------------|
| Client Vigibot | `/usr/local/vigiclient/` |
| Log principal | `/var/log/vigiclient.log` |
| Service systemd | `/etc/systemd/system/vigiclient.service` |
| Drop-in encode | `/etc/systemd/system/vigiclient.service.d/encode.conf` |
| Samples 40-pin | `/app/40pin_samples/` |
| Modèles BPU | `/opt/hobot/model/x5/basic/` |
| Doc POC | `docs/` (repo vigibot-rdk-x5) |
