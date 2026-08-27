<img src="brand/xcam-mark-512.png" width="72" align="left" hspace="12" vspace="4">

# XCam

Use an Android phone as a Windows webcam — and record what the sensor actually
gave, not what the cable could carry.

Android telefonu Windows'ta web kamerası olarak kullan — ve sensörün gerçekten
verdiğini kaydet, kablonun taşıyabildiğini değil.

<br clear="left">

> **Beta.** It works and it has been measured, but it has been run on one phone
> (Xiaomi 17 Pro) and one PC. Expect rough edges.
>
> **Beta.** Çalışıyor ve ölçüldü, ama tek telefonda (Xiaomi 17 Pro) ve tek
> bilgisayarda denendi. Pürüz bekle.

---

## English

### What it does

- Appears as **XCam Virtual Camera** in Zoom, OBS, Discord, Chrome, Teams.
- Streams over **USB** (~1 ms) or **Wi-Fi**, up to 4K.
- **Records at full sensor quality on the phone** while streaming a smaller
  picture to the PC. The call gets a webcam; the file gets the camera.
- A **manual panel**: ISO, shutter, white balance, focus, EV, zoom, log profile,
  LUT, cinematic matte, mirror, zebras, focus peaking.
- **Pre-roll**, so a take can start before you pressed the button.
- **Follow focus** with two marks and a timed pull.
- **Multicam**: several phones, one cut, each recording its own file.

### Install

1. Download the latest **[Release](../../releases)** — `XCam-Setup.zip` and
   `xcam.apk`.
2. On the PC, unzip and run:

   ```powershell
   .\install.ps1
   ```

   No administrator needed. The camera registers for your account only.

3. Install the APK on the phone and open it:

   ```bash
   adb install -r xcam.apk
   ```

4. Plug the phone in, allow USB debugging, press **Start capture** on the phone,
   then start **XCam** on the PC.

To remove it: `.\install.ps1 -Uninstall`. Your settings and recordings stay.

### Over Wi-Fi

The cable needs nothing. Wi-Fi is **off until you turn it on**, and off means
the phone is not listening on the network at all -- not filtered, not hidden:
the socket is bound to loopback, so nothing on the network can reach it.

Switch on **Allow Wi-Fi connections** in the phone app and it shows a six-digit
pairing code. Type that into the desktop app once, under **Settings → Pairing
code**, and that PC stays paired. Anything that connects without it is told
nothing and closed.

Leave Wi-Fi off on a network you do not trust. **New code** in the phone app
un-pairs every PC.

### Build from source

```powershell
.\tools\build-windows.ps1     # the desktop app and the virtual camera
.\tools\build-android.ps1     # the phone app; add -Install to push it
```

Needs Visual Studio 2022 (C++ desktop workload), the Windows SDK, and a JDK 17+
for the Android side.

### Documentation

- **[docs/MANUAL.md](docs/MANUAL.md)** — everything: how it works, what was
  measured, every setting and why it exists.
- **[docs/PROTOCOL.md](docs/PROTOCOL.md)** — the wire format.
- **[docs/BRAND.md](docs/BRAND.md)** — the mark and the palette.

---

## Türkçe

### Ne yapıyor

- Zoom, OBS, Discord, Chrome ve Teams'te **XCam Virtual Camera** olarak çıkar.
- **USB** (~1 ms) veya **Wi-Fi** üzerinden, 4K'ya kadar yayın yapar.
- Yayın sürerken **telefonda tam sensör kalitesinde kaydeder**. Görüşme bir web
  kamerası alır; dosya asıl kamerayı alır.
- **Manuel panel**: ISO, enstantane, beyaz ayarı, odak, EV, zoom, log profili,
  LUT, sinematik kaşe, ayna, zebra, odak zirvesi.
- **Ön-tampon**: çekim, düğmeye basmadan öncesini de içerir.
- İki işaret ve süreli geçişle **takip odağı**.
- **Çoklu kamera**: birkaç telefon, tek kesme, her biri kendi dosyasını kaydeder.

### Kurulum

1. Son **[Release](../../releases)** dosyalarını indir — `XCam-Setup.zip` ve
   `xcam.apk`.
2. Bilgisayarda zip'i aç ve çalıştır:

   ```powershell
   .\install.ps1
   ```

   Yönetici gerekmiyor. Kamera yalnızca senin hesabına kurulur.

3. APK'yı telefona kur ve aç:

   ```bash
   adb install -r xcam.apk
   ```

4. Telefonu tak, USB hata ayıklamasına izin ver, telefonda **Start capture**'a
   bas, sonra bilgisayarda **XCam**'i başlat.

### Wi-Fi üzerinden

Kablo için hiçbir şey gerekmiyor. Wi-Fi **sen açana kadar kapalı**, ve kapalı
demek telefonun ağı hiç dinlemediği demek — filtrelenmiş ya da gizlenmiş değil:
soket loopback'e bağlı, yani ağdaki hiçbir şey ona ulaşamaz.

Telefon uygulamasında **Wi-Fi bağlantılarına izin ver**'i açtığında altı haneli
bir eşleştirme kodu gösterir. Onu masaüstü uygulamasında bir kez
**Ayarlar → Eşleştirme kodu** alanına yaz; o bilgisayar eşleşmiş kalır. Kodu
bilmeden bağlanan hiçbir şeye tek kelime söylenmez, bağlantısı kapatılır.

Güvenmediğin bir ağda Wi-Fi'ı kapalı bırak. Telefondaki **Yeni kod**, eşleşmiş
bütün bilgisayarların eşleşmesini bozar.

Kaldırmak için: `.\install.ps1 -Uninstall`. Ayarların ve kayıtların kalır.

### Kaynaktan derleme

```powershell
.\tools\build-windows.ps1     # masaüstü uygulaması ve sanal kamera
.\tools\build-android.ps1     # telefon uygulaması; -Install ile doğrudan yükler
```

Visual Studio 2022 (C++ masaüstü bileşeni), Windows SDK ve Android tarafı için
JDK 17+ gerekiyor.

### Belgeler

- **[docs/MANUAL.md](docs/MANUAL.md)** — her şey: nasıl çalıştığı, neyin
  ölçüldüğü, her ayarın neden var olduğu.
- **[docs/PROTOCOL.md](docs/PROTOCOL.md)** — kablo formatı.
- **[docs/BRAND.md](docs/BRAND.md)** — marka ve renkler.

---

MIT licensed. See [LICENSE](LICENSE).
