# Maze Connect — devir notu

Bu belge, uzun bir oturumun sonunda yazıldı. Yeni bir sohbete başlarken
**önce bunu okut** — hem nerede kalındığını hem de tekrar keşfedilmesi
pahalıya patlayacak kararları içeriyor.

İki depo birlikte ilerliyor:
- `/run/media/berkkucukk/Backup/Projects/Maze-Connect` (masaüstü, Qt6/QML/C++)
- `/run/media/berkkucukk/Backup/Projects/Maze-Connect-Mobile` (Android, Kotlin/Compose)

Tam plan: `~/.claude/plans/robust-rolling-kazoo.md`

---

## 1. Uygulama ne yapıyor (ve ne yapmıyor)

Maze Connect **KDE Connect klonu değil.** Amaç, Maze Linux'u telefondan
yönetmek:

- Maze AI ile sohbet (yerel Ollama üzerinden)
- maze-guard killswitch'lerini açıp kapatmak
- maze-control-center benzeri bir durum panosu
- Bilgisayarda önceden tanımlanmış komutları çalıştırmak

Pano/bildirim yansıtma/ping gibi KDE Connect işleri **kaldırıldı** — onları
yapan zaten var. **Dosya transferi kaldı.**

---

## 2. Şu an nerede kalındı

### Bitmiş ve doğrulanmış

**Aktarım katmanı (değişmiyor, sağlam):**
- Karşılıklı TLS 1.3 (min=max sabitlenmiş), açık anahtar pinleme
- SAS eşleştirme (6 hane), MITM testle kanıtlı
- Replay penceresi, çerçeve boyut sınırları, dosya yolu hapsi
- Eşleşmemiş peer yalnızca eşleşme mesajı gönderebilir
- Yeniden bağlanma (uygulama yeniden başlayınca prompt'suz)
- 13 masaüstü test paketi + 58 mobil test

**Gerçek cihazda uçtan uca doğrulandı:** masaüstü ile Android emülatörü
gerçekten eşleşti, iki ekranda aynı kod (`358329`) çıktı, iki taraf da
birbirinin açık anahtarını pinledi.

**Arayüz:** Maze uygulama ailesinin diline geçirildi (çerçevesiz pencere,
cam panel, sol sidebar, KDE blur, tek örnek kilidi).

**Paketleme:** Arch paketi + imzalı APK üretiliyor.

### Adım 1 — ✅ bitti (2026-08-01)

Plan'daki 1. adım (eski özellikleri kaldır) tamamen kapandı:

| | Durum |
| --- | --- |
| Masaüstü kod temizliği | ✅ bitti, QML uyarısız |
| Mobil kod temizliği | ✅ kalıntı yok, flavour'lar kalktı (`app/src/main` tek başına) |
| `PROTOCOL_VERSION` → 2 | ✅ iki tarafta da |
| `SecretDetector` kaldırıldı | ✅ iki tarafta da, derleme girdileriyle birlikte |
| Mobil `assembleDebug test lint` | ✅ `clean` sonrası, 35/35 test, lint temiz |
| Masaüstü `ctest` | ✅ 9/9 |

Doğrulanan son hâl: mobilde `VersionTest.protocolVersionMatchesDesktop`
geçiyor (protokol sürümü 2). `SecretDetector` yalnızca pano sızıntı filtresiydi
ve pano özelliği kalktığı için hiçbir çağıranı kalmamıştı; masaüstü
`core/src/plugins/` dizini şimdi boş (`.gitkeep` duruyor) — Adım 2–5'in
sağlayıcıları oraya gelecek.

> Not: bu iki depo **git altında değil.** Silinen dosya geri gelmiyor.
> `SecretDetector` dosyalarının kopyası yalnızca o oturumun scratchpad'inde
> tutuldu, kalıcı değil. İlk fırsatta `git init` edilmesi iyi olur.

---

### Adım 2 — ✅ kod tamam, gerçek cihaz turu bekliyor (2026-08-01)

Yazılanlar:

| | Nerede |
| --- | --- |
| Durum betiği | `packaging/maze-connect-status` → `/usr/lib/mazeconnect/` |
| Masaüstü sağlayıcı | `core/src/plugins/StatusProvider.cpp` |
| Mesajlar | `statusRequest` / `statusReport` (iki tarafta) |
| Yetenek | `systemStatus`, **varsayılan kapalı**, cihaz başına |
| Masaüstü pano | `app/qml/views/DashboardView.qml` + `components/Meter.qml` |
| Mobil ayrıştırıcı | `core/.../protocol/SystemStatus.kt` |
| Mobil pano | `app/.../ui/screens/DashboardScreen.kt` |

Doğrulanan: masaüstü 10/10 ctest (yeni `tst_statusprovider`, 8 vaka),
mobil 45/45 (yeni `SystemStatusTest`, 8 vaka), lint temiz, qmllint yeni
dosyalarda uyarısız, `cmake --install` doğru yerlere koyuyor, imzalı APK
üretiliyor ve manifest'te bildirim dinleyicisi yok.

Emülatörde ayrıca: uygulama açıldı, yeni **Dashboard** sekmesi çalışıyor,
eşleşme yokken doğru boş durumu ve doğru gerekçeyi gösteriyor
("Pair with a computer first").

**Doğrulanmayan (kullanıcı kendi cihazlarında yapacak):** telefon–masaüstü
eşleşmesi kurulduktan sonra panonun *gerçek veriyle* dolması, yani
Kotlin↔C++ `statusRequest`/`statusReport` turunun canlıda çalışması ve
masaüstü panosunun çizilmesi. Wayland'de masaüstü butonlarına sentetik
tıklama yok, eşleştirme onayına basılamadı.

Tur için:
```bash
# Masaüstü: betiği kurmadan denemek için
MAZECONNECT_STATUS_HELPER=$PWD/packaging/maze-connect-status ./build/app/maze-connect
```
1. Telefonda Devices → adresle eşleştir, iki tarafta da kodu onayla.
2. **İki tarafta da** Dashboard anahtarını aç: masaüstünde Devices
   kartındaki "Dashboard", telefonda cihaz satırındaki "Dashboard".
   Tek taraf yetmez — yetenek iki uçta da açık olmak zorunda.
3. Masaüstünde Ctrl+3, telefonda Dashboard sekmesi.

Beklenen: iki ekranda da aynı makinenin CPU/bellek/disk ölçerleri, güvenlik
servisleri, ağ satırları ve sertleştirme skoru. maze-tools kurulu değilse
"kullanılamıyor" der, çökmez.

#### Bu adımda öğrenilenler

1. **`maze_status.cpu_usage_percent()` durum tutuyor.** İlk çağrı taban
   ölçümü alıp `0.0` döner. Tek atımlık bir betikte bu, CPU'nun sonsuza
   kadar %0 görünmesi demekti; yardımcı önce bir ölçüm alıp 0.25 sn bekliyor.
2. **`hello`, desteklenen yetenekleri değil `defaultEnabledCapabilities()`'i
   duyuruyormuş.** FileTransfer ikisinde de olduğu için fark edilmiyordu.
   Artık ayrı: `supportedCapabilities()` duyurulur, `defaultEnabled...`
   yalnızca yeni eşleşmede saklanır. Aynı ayrım mobilde `Capability.SUPPORTED`.
3. **Rapor, istenmediyse kabul edilmiyor.** Eşleşmiş olmak tek başına
   ekrana içerik koyma yetkisi değil — iki tarafta da "sorduk mu" kümesi var.
4. Anlık görüntü, telefonda `SystemStatus.parse` ile alan alan sınırlanıyor
   (uzunluk, kontrol karakteri, satır sayısı, yüzde kelepçesi). Bağlantı
   kimliği doğrulanmış ama karşı makine güvenilir değil.
5. **Üçüncü sekme mobil masthead'i taşırdı.** Emülatörde ilk açılışta
   "SETTINGS" ikiye bölündü; birim testinde görülmeyecek bir şeydi.
   Wordmark ve sekmeler artık ayrı satırda, sekme satırı yatay kaydırılabilir
   — Adım 3–5 üç sekme daha ekleyecek.
6. **`build.sh` bütün `packaging/` dizinini dışlıyordu.** İçinde sadece
   PKGBUILD varken doğruydu; artık `maze-connect-status` ve `icons/` de
   orada. Hata `check()` yeşil geçtikten *sonra*, `package()` aşamasında
   patlıyor ve kaynak ağacını işaret ediyor — `build.sh`'ı değil. Dışlama
   kaldırıldı, sebebi dosyanın içine yazıldı.

### Belgeler (2026-08-01)

`README.md`, `docs/PROTOCOL.md` ve `docs/THREAT_MODEL.md` iki depoda da
Adım 1'den kalmıştı: hâlâ pano senkronu, bildirim yansıtma, ping ve
kaldırılmış `notifications` flavour'ının derleme komutlarını anlatıyorlardı.
PROTOCOL.md'deki `hello` örneği hiç uygulanmamış bir şemaydı (`protocolVersion`
alanı, gerçek `v`/`t`/`c` zarfı yerine). Hepsi v2'ye çekildi; `statusRequest`/
`statusReport` ve yetenek tablosu artık belgeli.

### Logo (2026-08-01)

Kaynak: `Maze-Connect/packaging/icons/maze-connect.png` (1000×1000).
Uygulamanın artık kendi markası var; sidebar ve filigran **maze-branding'in
ortak Maze logosunu kullanmıyor** (PKGBUILD'deki o optdepend kaldırıldı).

İki biçim var ve ayrı işler yapıyorlar — birini diğerinin yerine koymayın:

| Dosya | Nerede kullanılır |
| --- | --- |
| `maze-connect.png` (siyah yuvarlak kare zeminli) | uygulama ikonu — hicolor 48/64/128/256/512, Android launcher |
| `maze-connect-mark.png` (beyaz, saydam zemin) | `/usr/share/pixmaps/`, QML sidebar + köşe filigranı |

Zeminli olan başlatıcıda rastgele duvar kâğıdının üstüne düşüyor, kendi
zeminini taşımak zorunda. Zeminsiz olan panelin *üstüne* çiziliyor; orada
siyah kare panelde delik gibi görünürdü.

Android tarafı: uyarlanabilir ikon (`drawable-*/ic_launcher_foreground.png`,
iç 72dp güvenli alana yerleştirilmiş) + eski başlatıcılar için
`mipmap-*/ic_launcher.png` ve `ic_launcher_round.png`. Daire ve squircle
maskeleri denendi, kırpılma yok. `monochrome` katmanı da aynı ön plan, yani
temalı ikonlar da çalışıyor.

Yeniden üretmek gerekirse ImageMagick ile: parlaklık kanalı doğrudan alfa
olarak kullanılıyor (`-colorspace gray` → `CopyOpacity`), çünkü çizim zaten
siyah üstüne beyaz.

---

### Sabit port + firewalld (2026-08-01)

**TCP portu artık sabit: 38271** (`Server::kDefaultPort`), keşif beacon'ıyla
aynı numara. Önceden ephemeral'dı ve bu iki şeyi bozuyordu — firewall kuralı
yazılamıyordu, ve "adresle eşleştir" dizesi her açılışta değişiyordu (tam da
keşfin zaten başarısız olduğu durumda). Port alınmışsa ephemeral'a düşüyor,
yani ikinci bir örnek yine kalkıyor; `tst_transport` bunu çiviliyor.

Paket `usr/lib/firewalld/services/maze-connect.xml` kuruyor ve
`packaging/maze-connect.install` scriptlet'i onu **varsayılan bölgeye**
ekliyor, kaldırırken çıkarıyor. firewalld yoksa sessizce geçiyor. Geri almak:
```bash
firewall-cmd --permanent --remove-service=maze-connect && firewall-cmd --reload
```

### Adım 3 — ✅ `commands` yazıldı (2026-08-01)

| | Nerede |
| --- | --- |
| Çalıştırıcı | `core/src/plugins/CommandRunner.cpp` |
| Dosya | `~/.config/mazeconnect/commands.json`, **yalnızca sahibi okuyabilir** |
| Mesajlar | `commandList`/`commandCatalog`, `commandRun`/`commandResult` |
| Yetenek | `commands`, varsayılan kapalı |
| Masaüstü | `app/qml/views/CommandsView.qml` — argv'yi tam gösterir |
| Mobil | `ui/screens/CommandsScreen.kt` — `confirm` işaretliyse bir kez daha sorar |

Çivilenen davranışlar (`tst_commandrunner`, 13 vaka):
- **Hiçbir yerde shell yok.** argv, QProcess'e program + argüman listesi
  olarak veriliyor; `;`, `&&`, `$( )`, backtick hiçbir anlam taşımıyor —
  filtrelendikleri için değil, onları yorumlayacak bir shell olmadığı için.
- Komut-şeklinde id (`lock; touch /tmp/...`) dahil 7 deneme reddediliyor.
- Tam eşleşme: `LOCK`, `lock ` ve boş id çalışmıyor.
- Dosyayı başkası okuyabiliyorsa katalog tamamen reddediliyor.
- Yinelenen id varsa dosya bütün olarak reddediliyor — "hangisi" sorusu
  tahminle cevaplanmıyor.
- Çıktı 16 KiB'de kesiliyor, komut 30 sn'de öldürülüyor, en fazla 4 eşzamanlı.

**Katalogda `argv` yok.** Telefon etiket ve id görür; ne çalıştığını yalnızca
bilgisayar bilir. İnterop testi bunu iki tarafta da çiviliyor.

Masaüstünde Commands ekranı "başlangıç dosyası oluştur" düğmesi sunuyor —
mevcut dosyanın **üzerine yazmaz**.

### Tasarım (2026-08-01)

- Mobilde gezinme **alta taşındı** (`BottomNav`). Üstteki sekme satırı ikide
  tutuyordu, üçte "SETTINGS" sarıyordu, ve telefonun üstü başparmağın
  ulaşamadığı yer. Aktif sekme renkle değil üstündeki çizgiyle işaretli —
  paletin vurgu rengi yok.
- Masaüstü sidebar'ına Commands, Maze AI ve Guard eklendi; kısayollar Ctrl+1..8.
- Mobilde alt gezinme beş sekme; cihaz kartındaki yetenek anahtarları yatay
  kaydırılabilir (üç anahtar 1080px'e sığmıyordu).

> Başsız emülatörde ekran görüntülerinin en üstünde alt gezinmenin bir kopyası
> görünüyor. `uiautomator dump` görünüm ağacında orada hiçbir şey olmadığını
> söylüyor — swiftshader artefaktı, kod hatası değil. Kovalamayın.

---

### Adım 5 — ✅ Maze AI sohbeti yazıldı (2026-08-01)

| | Nerede |
| --- | --- |
| Köprü | `core/src/plugins/AiBridge.cpp` — `QNetworkAccessManager`, NDJSON |
| Ollama | `http://127.0.0.1:11434`, `$MAZECONNECT_OLLAMA_HOST` testler için |
| Mesajlar | `aiModels`/`aiModelList`, `aiPrompt` → `aiChunk`* → `aiDone` |
| Yetenek | `ai`, varsayılan kapalı |
| Masaüstü | `app/qml/views/AiView.qml` |
| Mobil | `ui/screens/AiChatScreen.kt` |

**Sohbet, ajan değil — ve bu bir eksiklik değil, karar.** `AiBridge` içinde
QProcess yok, araç kataloğu yok, birine ulaşabilecek bir dal da yok.
Gönderilen sistem istemi Maze AI'ın kendi `CHAT_ONLY` kişiliği (birebir
taşındı). `tst_aibridge::neverOffersTools` gönderilen baytlara bakıp
`tools`/`functions` anahtarının olmadığını doğruluyor — dosya düzenlendikçe
doğru kalmasına güvenilmiyor, ölçülüyor.

**Konuşma bilgisayarda durur, cihaz başına.** Telefon tek satır metin
gönderir; geçmiş göndermez, sistem istemi göndermez. Yani kişiliği
değiştiremez ve "daha önce şunu demiştin" diye geçmişi kurgulayamaz. Bütçe
24000 karakter, en eskiden kırpılır (Maze AI'daki gibi).

Model adı da telefondan gelir ama **Ollama'nın bildirdiği listede yoksa
reddedilir** — peer'ın seçtiği bir dize Ollama'nın API'sine olduğu gibi
gitmez.

`tst_aibridge` (14 vaka) kapsadıkları: NDJSON ayrıştırma, **bir JSON satırının
iki pakete bölünmesi** (klasik akış hatası; tek pakete sığdığında hiç
görünmez), yarıda kesilen akışın yine de tamamlanması, ulaşılamayan Ollama ile
modelsiz Ollama'nın ayrı mesajlar vermesi, cihazlar arası geçmiş sızmaması.

> Gerçek Ollama'ya karşı da elle doğrulandı (gemma3): satır başına
> `message.content`, son satırda `done:true` ve boş içerik — ayrıştırdığımızla
> birebir aynı.

### Adım 4 — ✅ `guardControl` yazıldı (2026-08-01)

| | Nerede |
| --- | --- |
| Köprü | `core/src/plugins/GuardBridge.cpp` |
| Soket | `/run/maze/guard.sock`, `$MAZECONNECT_GUARD_SOCKET` testler için |
| Mesajlar | `guardStatus`/`guardReport`, `guardRequest`/`guardResult` |
| Yetenek | `guardControl`, varsayılan kapalı |
| Masaüstü | `app/qml/views/GuardView.qml` (banner dahil) |
| Mobil | `ui/screens/GuardScreen.kt` (iki yönde de onay sorar) |

**`PANIC` ve `RESTORE` engellenmedi — yoklar.** Onları üretebilecek bir kod
yolu bulunmuyor, ve `guardRequest` mesajında yazılabilecekleri bir alan da
yok (fiil alanı yok; cihaz adı + boolean). `tst_guardbridge` bunu sokete
yazılan baytlara bakarak doğruluyor: her cihaz, iki yön, artı bir STATUS
sürülüyor ve gönderilen her satırın ya `STATUS` ya `KILL ` ile başladığı
teyit ediliyor.

Diğer sabitlenen davranışlar (11 vaka):
- Cihaz **C++ enum**. Peer'ın gönderdiği ad sabit tabloda aranır ve enum'a
  çevrilir; komut satırına asla metin olarak girmez. Yani
  `"camera on\nPANIC"` bir cihaz adı değildir — ikinci satır enjekte
  edilemez.
- `none` (makinede o donanım yok) ≠ `off`. Ayrı durum olarak korunuyor;
  aksi hâlde telefon var olmayan bir korumayı varmış gibi gösterirdi.
- Değişiklikten sonra durum **yeniden okunur**. Broker'ın kabul etmesi
  donanımın uyduğunun kanıtı değil, o yüzden istenen değil gerçekleşen
  bildiriliyor.
- `ERR unauthorized` (aktif yerel oturum yok) kullanıcıya iletilir, başarı
  gibi gösterilmez.

**Her ayrıcalıklı işlem duyurulur:** Activity günlüğüne yazılır *ve*
masaüstünde banner çıkar (`Backend::guardChangedRemotely`). Banner solmuyor —
üç saniyede kaybolan bir uyarı kaçırılabilir, ki bu da olma amacını ortadan
kaldırır.

> **Yol boyunca bulunan gerçek hata:** `GuardBridge` ilk yazımda tamamen
> bloklayıcıydı (`waitForReadyRead`). Test bunu ortaya çıkardı — sunucu olay
> döngüsüne muhtaç olduğu için bağlantı hiç kabul edilmiyordu — ama asıl
> sonucu gerçek kullanımda masaüstünün 15 sn donmasıydı: maze-guardd
> rfkill/systemctl çağırıyor ve kendine 60 sn bütçe veriyor. Asenkron kuyruk
> hâline getirildi.

### Yeniden bağlanma — düzeltildi (2026-08-01)

Kullanıcı bildirdi: ağdan düşüp geri bağlanınca mobilde pano gelmiyor. İki
ayrı hata çıktı.

**1. Kim arar kuralı yazı-turaydı.** Masaüstü yalnızca `m_deviceId <
device.deviceId` ise arıyordu (leksikografik eşitlik bozucu) ve **mobilde hiç
yeniden bağlanma kodu yoktu.** Telefonun rastgele UUID'si önce sıralanıyorsa
masaüstü aramayı reddediyor, telefon da hiç aramıyordu — bağlantı bir daha
kurulmuyordu. Eşleşmelerin yarısı düşen bir bağlantıdan hiç kurtulamıyordu.

Yeni kural, iki tarafta da aynı yorumla yazılı: **telefon bilgisayarı arar,
tersi olmaz.** Bilgisayar sabit uç — sabit adres, sabit port, hep dinliyor.
Telefon ağ değiştirir, adres değiştirir, arka planda bağlantı kabul
etmeyebilir; içeri aramak kırılgan yön. Aynı türden iki cihaz (iki masaüstü)
için eşitlik bozucu duruyor.

Mobile eklenenler: `reconnectPairedDevices()` süpürmesi, üç tetikleyiciyle —
beacon görüşü (saniyeler içinde), 10 sn'lik periyodik tarama (biz başlamadan
önce duyuran cihaz için), ve **ağ değişimi** (`ConnectivityManager`
callback'i, `MazeConnectService` içinde). Sonuncusu olmadan Wi-Fi döndükten
sonra 10 sn'ye kadar bekleniyordu — ki kullanıcının telefonu eline alıp
"bağlandı mı" diye baktığı an tam orası.

`Beacon.refresh()` de eklendi: `MulticastSocket` açıldığı arayüze bağlı
kalıyor, Wi-Fi gidip gelince o bağ bayatlıyor — soket yaşıyor, duyurular
gönderiliyor görünüyor, hiçbir şey ulaşmıyor. Yeniden bağlamak tek güvenilir
çözüm.

**2. Bekleyen iş kümeleri bağlantıyla birlikte temizlenmiyordu.** Her
"kim istedi" kümesi istenmemiş cevabı reddetmek için var; bağlantısından
sağ çıkan bir kayıt bunu tersine çeviriyor — **yeniden bağlandıktan sonraki
ilk gerçek cevap "istenmemiş" görünüp düşürülüyor.** Panonun bağlantı geri
geldiği hâlde boş kalmasının ikinci sebebi buydu.

En kötüsü `m_guardRequester`: tek slot, ve toggle ortasında kaybolan bir
cihaz onu sonsuza dek tutuyordu — ardından **her** cihazın **her** killswitch
isteği kalıcı olarak "another change is in progress" ile reddediliyordu,
yeniden başlatmadan dönüşü yok.

İki tarafta da `forgetPendingWork(deviceId)` eklendi.
`tst_devicemanager::aDroppedLinkDoesNotLeavePendingWorkBehind` bunu gerçek
bir kopma/yeniden bağlanma turuyla çiviliyor.

### Mobilde dosya alımı + widget + senkron optimizasyonu (2026-08-01)

**Dosya alımı artık var.** Masaüstündeki katmanlı savunmalar birebir taşındı
(`core/.../filetransfer/FileTransferReceiver.kt`):
ad tek güvenli bileşene indirgenir, baytlar önce rastgele adlı geçici dosyaya
yazılır, yazılan bayt **bildirilen boyuttan bağımsız** sayılır, ve rename'den
sonra yol yeniden çözülüp inbox içinde olduğu doğrulanır. Teklif asla otomatik
kabul edilmez.

Inbox: `Android/data/.../files/Download/inbox`. Hiçbir API'de runtime izni
gerektirmez, dosya yöneticisinden görünür, kaldırılınca gider. "Aç" düğmesi
`FileProvider` URI'si veriyor — dizini değil, kullanıcının seçtiği tek dosyayı
paylaşır (`res/xml/file_paths.xml` kapsamı **sadece** inbox).

`FileReceiverTest` (9 vaka): yol geçişi adları, bildirilen boyutu aşan
gönderici, yarıda kesilen transfer, aynı adın üzerine yazamama, eşzamanlılık
sınırı, ve inbox dışına çözülen sembolik bağ.

> Sembolik bağ testinde beklentim yanlıştı, kod değil: `rename(2)` bağı takip
> etmez, yerine geçer — yani dosya güvenli şekilde inbox'a düşüyor. Test
> gerçek güvenlik özelliğini ("dışarı hiçbir şey düşmez") ölçecek şekilde
> düzeltildi, geçsin diye zayıflatılmadı.

**Senkron optimizasyonu: `statusUnchanged`.** Pano birkaç saniyede bir soruyor
ve boştaki bir makinenin okuması her seferinde aynı. Masaüstü artık gönderdiği
son anlık görüntünün SHA-256'sını **cihaz başına** tutuyor; değişmemişse ~2 KB
yerine sadece zarfı gönderiyor. Bağlantı koptuğunda bu kayıt da siliniyor —
yoksa yeniden bağlanan cihaza "değişmedi" denirdi ve elinde hiçbir şey olmazdı.

**Widget.** `app/.../widget/` — ana ekranda hostname + üç ölçer. Launcher'ın
sürecinde çizildiği için bağlantıya erişemez; uygulamanın gördüğü son anlık
görüntü diske yazılıyor (`WidgetSnapshotStore`) ve widget onu çiziyor.
**Kaç dakikalık olduğunu da yazıyor** — zaman damgasız eski değerler canlıymış
gibi görünür ki bu hiç göstermemekten kötüdür. Sadece hostname ve ölçerler
saklanıyor; yerel IP, çekirdek, donanım, güvenlik servisleri **diske
yazılmıyor**. Cihaz eşleşmeden çıkarılınca veya yetenek kapatılınca temizleniyor.
`updatePeriodMillis=0`: sistemin 30 dakikalık tabanı yerine anlık görüntü
geldikçe yeniden çiziliyor.

### Kullanım geri bildirimi sonrası düzeltmeler (2026-08-01)

Kullanıcı emülatörde denedi ve dört şey söyledi. Hepsi haklıydı.

**1. Yetenek anahtarları kaldırıldı; varsayılan artık "hepsi açık".**
Yeni eşleşen bir telefon dört ekranda sonsuza dek "asking the computer…"
gösteriyordu, çünkü dört yetenek kapalıydı ve bunu hiçbir yerde söylemiyordu.
Eşleştirme zaten *asıl* karar — iki ekranda altı hane karşılaştırılıp iki
tarafta onaylanıyor. Ardına bir duvar daha koymak karar eklemedi, sadece
uygulamayı bozuk gösterdi.

`defaultEnabledCapabilities()` artık `supportedCapabilities()`. **Buna
`guardControl` de dahil** — eşleşmiş telefon killswitch çevirebiliyor. Onu
hâlâ tutan şeyler: PANIC/RESTORE protokolde hiç yok, cihaz tablosu sabit
enum, telefon iki yönde de onay soruyor, her değişiklik isteyen cihazla
birlikte Activity'ye yazılıyor, masaüstünde solmayan banner çıkıyor. **Geri
alma masaüstünün Devices sayfasında** — kontrol edilen makine orası.

Mobilden "Allow" satırı tamamen kalktı.

**2. Sessiz ret ortadan kalktı.** Asıl hata buydu ve genel hâli varsayılanla
sınırlı değildi: yetenek kapalıyken masaüstü isteği **sessizce yutuyordu**.
Sessizlik yavaş makineden ayırt edilemez, o yüzden telefon "asking…"de
kalıyordu. Artık her reddedilen istek kendi özelliğinin hata alanıyla
cevaplanıyor (`statusReport.error`, `commandCatalog.error`,
`aiModelList.error`, `guardReport.error`, `commandResult.error`,
`aiDone.error`, `guardResult.error`). `commandCatalog`'a bu yüzden `error`
alanı eklendi: boş liste tek başına "komut yok" ile "söylemiyorum"u ayırt
edemiyordu.

`capabilitiesStartGrantedAndStayRevocable` bunu çiviliyor: geri alındıktan
sonra cevap **geliyor**, gerekçeli ve içi boş.

**3. Alt gezinme sıkışıklığı.** Yedi sekme eşit sütunlara bölününce her biri
~150px alıyordu; yazılar "DASHBOA"/"SETTING" diye kesiliyor ve birbirine
giriyordu. Artık her öğe kendi metnine göre boyutlanıyor ve satır yatay
kaydırılıyor. "Maze AI" → "AI" (zaten Maze Connect'in içindesin).

**4. Masaüstünden Maze AI ekranı kaldırıldı.** Masaüstünde zaten Maze AI
uygulaması var; onu kopyalamak kurulu bir programın kötü bir kopyası olurdu.
**`AiBridge` duruyor** — bu uygulamanın kattığı şey o Ollama'ya *telefondan*
erişmek. Giden sadece pencere.

### Killswitch tersti — düzeltildi (2026-08-01)

Kullanıcı telefondan **Block**'a bastı, wifi **açıldı**. Activity günlüğü de
"asked for on — now on" diyerek tutarlı görünüyordu.

Sebep: `maze-guardd`'da `on`/`off` **cihazı** adlandırıyor, anahtarı değil.
`KILL wifi off` → `rfkill block`. Ben `on`'u "koruma açık" diye okumuşum,
bütün düğmeler ters çalışıyordu. İki okuma da tek başına doğru görünüyor —
asıl hata iki sözlüğü aynı boolean'a yüklemekti.

`setKill(device, on)` → **`setDeviceEnabled(device, enabled)`**;
`requestedOn` → `requestedEnabled`. Parametre artık anahtarın değil cihazın
adını taşıyor, `rfkill` ve `maze-guardd` ile aynı sözlük.
`blockingSendsOffAndAllowingSendsOn` yönü tele karşı çiviliyor.

Arayüzlerdeki çağrı `!blocked` idi — aynı ifade ters sözlükle okununca Block
unblock yapıyordu. İki tarafta da `blocked` oldu.

### Kullanıcı testinden çıkan diğer düzeltmeler (2026-08-01)

- **Widget tek bar gösteriyordu.** Üç satır tek bir satır düzeninin
  `<include>`'uydu; RemoteViews id'leri bütün ağaçta çözer ve include başına
  kapsam yoktur, o yüzden `setTextViewText(R.id.meter_label, …)` sadece
  ilkine ulaşıyordu. Satırlar ayrı id'lerle açıkça yazıldı. Ayrıca **2×1
  kompakt widget** eklendi (`DashboardWidget.Compact`).
- **Mobilden PC'ye dosya gönderme eklendi.** Sistem seçicisi (SAF), depolama
  izni yok. Dosya karşı taraf **kabul edene kadar açılmıyor** — reddeden bir
  bilgisayar telefona hiçbir okuma maliyeti çıkarmıyor.
- **AI sohbetinde klavye.** İç alana `imePadding()` eklendi; klavye açılınca
  tüm sütun yukarı kayıp transcript'i ekrandan atıyordu.
- **OLED teması ayarlardan kaldırıldı.** Maze zaten true-black; anahtar
  "gönderilen görünüm" ile "onun biraz açığı" arasında seçim sunuyordu.
  Masaüstü kendi anahtarını aynı gerekçeyle çoktan atmıştı.
- **`unlock` başlangıç komutlarına eklendi**, `confirm: true` ile — kilidi
  *açmak* korumayı kaldıran yön.
- `argumentsAreNeverReSplit`: bir argv elemanının içindeki `;`, `&&`, `$HOME`
  ve backtick'ler tek argüman olarak geçiyor, genişletilmiyor, çalıştırılmıyor.
  Komut çalıştırmada ikinci bir ayrıştırma yok çünkü shell yok.

### 0.4.0 — ikinci tur kullanıcı geri bildirimi (2026-08-01)

**Klavye hâlâ bozuktu, ve önceki "düzeltmem" onu kötüleştirmişti.**
`safeDrawingPadding()` IME'yi *zaten* içeriyor; üstüne `imePadding()` koyunca
klavye iki kez sayıldı ve masthead ile transcript ekranın üstünden dışarı
itildi. `imePadding()` kaldırıldı, ve klavye açıkken masthead/durum
satırı/alt gezinme gizleniyor — sütun normal boyunun üçte birine düşüyor,
kalanı transcript'e gitmeli. Gezinme zaten klavyenin arkasında kalıyordu.

**Widget'lar boştu, iki ayrı sebeple:**
1. Anlık görüntü *yalnızca* Dashboard sekmesi açıkken kaydediliyordu. Oraya
   hiç girmeyen biri için hiçbir şey yazılmıyordu. `DeviceManager` artık 60
   sn'de bir arka planda okuyor — neredeyse bedava, çünkü karşı taraf 2 sn
   önbellekliyor ve değişmemişse `statusUnchanged` dönüyor.
2. İçerik gerçekten azdı. Büyük widget'a sertleştirme skoru ve "4/5 servis"
   satırı eklendi (isimler değil sayılar — ana ekran, bir makinenin hangi
   korumasının eksik olduğunu yayınlayacak yer değil).

**Yeni: kontrol widget'ı** (`ControlsWidget`, 4×2). Rapor değil **kumanda**:
her hücre kamera/mikrofon/wifi/bluetooth'u açıp kapatıyor, uygulamayı açmadan.
Uzaktan kumanda uygulamasının ana ekranda olmasının asıl sebebi bu.
`usb` kasten yok — telefondan USB engellemek yardım etmekten çok mahsur
bırakır. Uygulama çalışmıyorsa dokunuş sessizce yutulmuyor, uygulamayı açıyor.

Widget'lar artık üç boyutta: 4×2 pano, 2×1 kompakt, 4×2 kontroller.

**Masaüstündeki Files/Dashboard anahtarları kaldırıldı.** Her şey varsayılan
açık olduğundan artıktılar. Mekanizma duruyor (`DeviceStore` kümeyi tutuyor,
`DeviceManager` cevap verirken yeniden kontrol ediyor) — sadece arayüzü yok.
**Şu an geri alma arayüzü hiçbir tarafta yok**; gerekirse protokol
değişikliği olmadan geri getirilebilir.

**Başlangıç komutları 3'ten 14'e çıktı:** kilitle/kilidi aç, uptime, disk,
bellek, en yoğun süreçler, bekleyen güncellemeler, ağ adresleri, oturum
açanlar, sesi kıs/aç, uyut, yeniden başlat, kapat. Sonucu olan her şey
`confirm: true`.

---

## 3. Kalan adımlar

Plandaki beş adımın hepsi yazıldı. Kalanlar plan dışı — bkz. bölüm 8.

**Hiçbiri eşleşmiş bir bilgisayarla canlı denenmedi.** Emülatörde her ekran ve
her boş durum doğrulandı; veri akışı değil. Devir notunun kendi kuralı hâlâ
geçerli: gerçek cihazda çalıştırmadan bitti sayma.

## 4. Tekrar tartışılmaması gereken kararlar

### maze-guardd uzaktan erişimi kasten reddediyor
Kendi başlığında yazıyor:
> Callers are authorised by SO_PEERCRED: the peer must be a real user with an
> ACTIVE local session (mirrors polkit allow_active) — **not root, not remote**.

Maze Connect killswitch açıp kapatınca, **uzaktan erişimi reddetmek için
tasarlanmış bir broker'ın uzak vekili** oluyor. Kullanıcı bunu on/off için
kabul etti; savunulabilir kılan şey PANIC/RESTORE'un hiç bulunmaması,
yeteneğin varsayılan kapalı olması, sabit fiil tablosu ve günlükleme.

### Maze AI: sohbet, ajan değil
Maze AI agentic — JSON araç protokolüyle shell komutu çalıştırıyor. Maze
Connect'e **sohbet** giriyor, ajan değil. Sebep kullanıcının kendi kararı:
komutlar için "masaüstünde tanımlı allow-list" seçildi. Telefonun keyfi shell
çalıştıran bir ajanı sürmesi bu kararı dolanırdı. (Maze AI'ın kendisinde de
`MODE_CHAT` var — yani bu, onun desteklediği bir yapılandırma.)

### Eski protokolle konuşulmaz
Her sürüm mesaj kümesini değiştirdi. Sürüm uyuşmazlığında bağlantı
**reddedilir**, aşağı pazarlık yapılmaz.

**v3 (2026-08-01):** v2, mesaj kümesi 10 tipken ayarlanmıştı; Adım 2–5 onu
26'ya çıkardı. İkisi de "v2" deseydi aynı sürüm numarası farklı diller
anlatırdı — eski bir istemciye `commandList` ya da `statusUnchanged` gidince
bağlantı "bilinmeyen mesaj tipi" ile düşerdi. Kuralın vaat ettiği temiz ret
yerine kafa karıştırıcı bir kopma. Mesaj kümesi değiştiyse sürüm de değişir.

Pratik sonucu: **0.2.0 ile 0.3.0 eşleşemez.** İki tarafı da güncelleyin.

---

## 5. Pahalıya öğrenilen şeyler (tekrar düşmeyin)

Bunların hepsi gerçek cihaz testinde bulundu, birim testlerinde görünmüyordu:

1. **Android Keystore + TLS:** anahtar `DIGEST_NONE`'a izin vermezse her el
   sıkışma `Incompatible digest` ile düşer. Conscrypt ön-hash'lenmiş veriyi
   imzalatıyor. → `AndroidKeyStoreManager` içinde NONE + SHA256/384/512.
2. **`connection.start()` handler'lardan önce çağrılırsa** okuma döngüsü
   erken başlar ve karşı tarafın ilk mesajı sessizce düşer.
3. **`hello` ilk mesaj olmak zorunda.** İki ayrı coroutine'den gönderilirse
   sıra garanti değil. Masaüstü artık bunu zorunlu kılıyor.
4. **Kotlin `equals` StateFlow'u kırar.** Sadece `deviceId` karşılaştıran bir
   `equals`, kodu dolduran güncellemeyi "eşit" sayıp yayını bastırdı — kod
   üretiliyor, arayüz görmüyordu. Data class'larda tüm alanları karşılaştırın.
5. **QML animasyonu içerik gizlemesin.** Doğrulama kodu `opacity: 0`'da
   kalmıştı; diyalog boş kod alanıyla açılıyordu. Dekoratif her şey
   "görünür"e düşmeli, "gizli"ye değil.
6. **Boş panel ≠ sessiz makine.** Bir sonda çökerse ya da maze-tools yoksa,
   veri gösterememekle "hiçbir şey çalışmıyor" ekranda birebir aynı görünür.
   Bu yüzden her başarısızlık *sebebiyle* söyleniyor, hiçbir alan makul bir
   varsayılana düşürülmüyor, ve `unknown` üçüncü bir durum olarak korunuyor
   (kurulu olmayan servis, kapalı servis değildir).

### Play Protect
`NotificationListenerService` bildiren APK'lar yan yüklemede engelleniyor
(185 pazar). Play Store yolu etkilenmiyor — KDE Connect bu yüzden sorunsuz.
Bildirim yansıtma kalktığı için **artık bu sorun yok, tek APK yeterli.**

### Qt/Android kısıtları
- Qt6 `QSslKey`'de Ed25519 yok; ayrıca Android Keystore Ed25519'u donanımda
  tutmuyor → **EC P-256** seçildi. SPKI DER iki platformda birebir aynı.
- Ne Qt ne Android RFC 5705 TLS exporter'ı açıyor → SAS açık anahtarlara
  bağlı. MITM tespiti korunuyor.
- İki taraf da aynı sabit girdi için `876154` üretmek zorunda (interop testi).

---

## 6. Derleme ve dağıtım

Güncel sürüm **0.4.0** (mobil `versionCode = 4`), protokol **v3**. Sürüm üç yerde tutuluyor ve
elle senkron kalmaları gerekiyor: `CMakeLists.txt`'teki `VERSION`,
`packaging/PKGBUILD`'deki `pkgver`, ve mobil `app/build.gradle.kts`'teki
`versionName`/`versionCode`. `versionCode` asla geri gitmemeli — Android
daha düşük veya eşit koda sahip bir güncellemeyi kurmaz.


```bash
# Masaüstü
cd Maze-Connect
cmake -B build -S . -G Ninja && cmake --build build
ctest --test-dir build
./build.sh                    # -> dist/maze-connect-0.4.0-1-x86_64.pkg.tar.zst

# Mobil
cd Maze-Connect-Mobile
./gradlew assembleDebug test lint
./gradlew assembleRelease     # -> dist/maze-connect-0.4.0.apk (imzalı)
```

**İmzalama anahtarı** (git'te değil, `.gitignore`'da):
`keystore/maze-connect-release.jks` + `keystore.properties`.
Parola `keystore.properties` içinde durur ve **hiçbir belgeye yazılmaz** — bu
depo herkese açık. (Eski bir sürümü parolayı burada düz metin olarak taşıyordu;
2026-09-25'te kaldırıldı, parola değiştirilmeli.)
**Yedekleyin** — kaybolursa mevcut kurulumların üzerine güncelleme yapılamaz.

---

## 7. Doğrulama alışkanlığı

Bu projede işe yarayan yöntem: **gerçek cihazda çalıştırmadan bitti sayma.**
Yukarıdaki 5 hatanın hiçbiri birim testinde görünmüyordu.

- Emülatör: `~/Android/Sdk/emulator/emulator -avd haze_test -no-window -no-audio -gpu swiftshader_indirect`
- Masaüstünü emülatöre görünür kılmak: `adb reverse tcp:<port> tcp:<port>`,
  telefonda "adresle eşleştir" alanına `127.0.0.1:<port>`
- Masaüstü portu: `ss -tlnp | grep maze-connect`
- Masaüstü günlüğü: `QT_LOGGING_RULES="maze.connect.*.info=true"` —
  sadece `maze.connect.*=true` `qCInfo` satırlarını açmıyor.
- Wayland'de sentetik tıklama yok — masaüstü butonlarına basmak kullanıcıdan
  istenmeli; mobilde `adb shell input tap` çalışıyor.
- QML'i tıklamadan sınamak için: `QT_QPA_PLATFORM=offscreen` ile başlatın ve
  stderr'e bakın. Çalışma zamanı bağlama hatalarını yakalar (qmllint'in
  kaçırdıklarını), ama görünür olmayan sekmeler hiç kurulmaz — pano orada
  denenmiş sayılmaz.
- `XDG_DATA_HOME=<tmp>` ile başlatırsanız gerçek kimliğinizi ve eşleşmelerinizi
  kirletmeden deneyebilirsiniz.
- Guard değişikliğini `maze-guard status` ile teyit edin; sadece kendi
  arayüzümüze bakmak yeterli değil.

---

## 8. Ayrıca bekleyen (plan dışı)

- ~~Mobilde dosya alımı yok~~ — **yapıldı.** İki yönde de çalışıyor;
  `FileTransferReceiver.kt` geçici dosyaya yazıp bağımsız bayt sayısıyla
  doğruluyor, ad çakışmasını yeniden adlandırarak çözüyor.
- **BiometricPrompt** yok — eşleştirme silme / yeniden eşleştirme / kimlik
  silme için kapı olmalı. Threat model'de "not yet implemented" işaretli.
- **`dist/` içinde 0.1.0 artıkları duruyor** — iki depoda da. Mobilde ayrıca
  kaldırılmış `notifications` flavour'ından kalma `*-notifications.{apk,aab}`
  var; artık üretilemiyorlar. Silinebilirler, dokunulmadı.
- ~~qmllint uyarıları~~ — **temizlendi.** Üç görünüme
  `pragma ComponentBehavior: Bound` eklendi; `TransfersView`'daki `state`
  rolü `transferState` oldu (`QQuickItem::state`'i gölgeliyordu ve o bir
  string, yani ikisi sessizce farklı şeyler ifade edebilirdi). QML artık
  sıfır uyarı veriyor.
- **`tst_transport::rejectsUnpairedPeer` hâlâ ara sıra düşüyor.** 2026-08-01
  paket derlemesinde bir kez düştü (10 sn'lik `QTRY` doldu, süit 30 sn sürdü).
  Kovalandı ve **yeniden üretilemedi**: tek başına 10/10, tam süit 3/3, tam
  CPU yükü altında geçiyor, `fakeroot` altında geçiyor, makepkg'nin
  `-O2 -flto` bayraklarıyla derlenince geçiyor, ve makine boştayken aynı
  paket derlemesi 10/10 geçti. Yani ortam değil, zamanlamaya duyarlı bir
  yarış — `Server::connectionRejected` bazen hiç gelmiyor. Bir dahaki
  düşüşte kovalanacak yer burası; `kWaitMs` yükseltmek belirtiyi gizler.


---

## 0.5.0 — bu turda değişenler

**Tepsi ikonu ve oturumla başlama.** `Tray` + `Autostart` eklendi. Bunun
sonucu olarak uygulama artık `QGuiApplication` değil **`QApplication`** —
`QSystemTrayIcon` QtWidgets'te yaşıyor. Pencereyi kapatmak artık
uygulamayı kapatmıyor (`setQuitOnLastWindowClosed(false)`), çünkü bu bir
belge değil bir **bağlantı**; telefonun eriştiği şeyi pencere düğmesiyle
sessizce sonlandırmak yanlış olurdu. Bağlantıyı gerçekten kesen tek şey
tepsi menüsündeki *Quit* ve orada bunu açıkça yazıyor. Tepsi yoksa
(`isAvailable()` false) hiçbiri uygulanmıyor — geri getirilecek yer
olmadan gizlemek pencereyi kaybetmek olurdu.

Autostart **bir kez** yazılıyor. "Bir kez"in muhasebesi girdinin kendi
varlığıyla değil ayrı bir işaret dosyasıyla tutuluyor
(`~/.config/mazeconnect/autostart-decided`): aksi hâlde girdiyi silen
kullanıcı her açılışta ezilirdi. Ayarlar'a kapatma anahtarı kondu.

Autostart girdisi `maze-connect --tray` çağırıyor ve main.cpp bu bayrağı
görünce pencereyi gizli açıyor. Bu bayrak olmadan yorum "tepside gizli
başlar" diyordu ama hiçbir şey bunu yapmıyordu — her oturum açılışında
pencere yüze fırlardı.

**Gelen dosyalar artık `~/Downloads/Maze Connect`'te**
(`XDG_DOWNLOAD_DIR` gözetilir), uygulamanın veri dizininde değil. Alt
klasör bilerek: alıcı gelen kutusunu 0700 yapıyor ve bunu Downloads'ın
kendisine uygulamak kullanıcının istemediği bir izin değişikliği olurdu.
`ensureInbox()` de artık **yalnızca kendi oluşturduğu** dizinin modunu
ayarlıyor.

**`build.sh` sürüm uyuşmazlığında duruyor.** Bu tam olarak bu turda
ısırdı: `CMakeLists.txt` 0.5.0'a çıkarıldı, `packaging/PKGBUILD` 0.4.0'da
kaldı ve paket eski adla üretildi — kendi üzerine kurulmayı reddedecek bir
paket. Artık ikisi ayrışırsa derleme başlamıyor.

Kenar çubuğu etiketi *Connect* → *Maze Connect*. Cihazlar sayfasındaki
yetenek anahtarları kaldırıldı; Ayarlar'daki "her şey kapalı gelir" metni
de buna göre yeniden yazıldı, çünkü artık doğru değildi.

Sürüm 0.5.0. 13 test süiti geçiyor, derleyici/qmllint uyarısı yok.


---

## 0.6.0 — bu turda değişenler

**İki örnek sorunu `SingleInstance`'ın kendisindeydi.** Soket sondası
300 ms'de yanıt alamazsa ikinci kopya bunu "çökmeden kalmış bayat soket"
sayıyor, `removeServer()` ile **canlı örneğin soketini siliyor** ve kendisi
dinlemeye başlıyordu. Sonuç tam da bu sınıfın engellemesi gereken şey: iki
çalışan kopya, iki pencere, iki tepsi ikonu — üstelik ilk örneğe artık
hiçbir şey ulaşamıyor. Otorite `QLockFile`'a taşındı; bayatlığı pid'e
bakarak çözüyor, meşgul bir sürecin ne kadar hızlı cevap verdiğine değil.
Soket yalnızca "öne gel" kanalı olarak kaldı.

**Autostart girdisi artık tazeleniyor.** `applyDefaultOnFirstRun` işaret
dosyasını görünce tümüyle çıkıyordu, yani girdinin *içeriği* onu yazan
sürüme çakılı kalıyordu — `--tray`'den önce yazılmış bir girdi her oturumda
pencereyi açmaya devam ediyordu ve hiçbir yükseltme yoluna dokunmuyordu.
Artık girdi duruyorsa yeniden yazılıyor. İçerik tazelemek yeniden
etkinleştirmek değil: kullanıcının sildiği girdi silinmiş kalıyor.

Pencere QML'de `visible: false` ile başlıyor ve C++ karar veriyor. Önce
görünür açılıp sonra gizleniyordu, bu da her oturum açılışında bir kare
boyunca pencerenin ekranda çakmasına yol açıyordu.

**Eşleşme kaldırma artık senkron.** Karşı taraf çevrimdışıyken kaldırırsanız
bildirim gidecek yer bulamıyordu ve o taraf sizi listelemeye devam ediyordu.
Artık eşleşmemiş bir eş, eşleşme dışı bir şey istediği anda — ki bu tam
olarak "kendini eşleşmiş sanıyor" demektir — `unpair` alıyor. Eşleşme
sürecindeki bir eş yalnızca pairing mesajları gönderdiği için bu dala hiç
girmiyor, yani koşulsuz göndermek güvenli.

Sürüm 0.6.0. 13 test süiti geçiyor, uyarı yok.


---

## 0.7.0 — bu turda değişenler

**"Could not reach that device" yalan söylüyordu.** `requestPairing` üç ayrı
*senkron* nedenle false dönüyordu — keşfedilmemiş, zaten eşleşmiş, zaten
eşleşme sürüyor — ve üçü de ağ sorunuymuş gibi tek cümleye çevriliyordu.
İnsanı ağında olmayan bir sorunu aramaya gönderiyor. Artık `PairingStart`
enum'u dönüyor ve her biri kendi cümlesini alıyor. Asıl neden genellikle
telefonun kendini duyurmayı bırakmış olmasıydı; mobil taraftaki `Link`
düzeltmesi onu kaynağında çözüyor.

**"Codes match" ölü düğme gibiydi.** Basınca hiçbir şey değişmiyordu, çünkü
eşleşme *iki* taraf da cevaplayınca tamamlanıyor ve arada geçen sürede ekran
aynı kalıyordu. `pairingAnswered` eklendi; cevaplandıktan sonra düğmeler
yerine "Waiting for the other device to confirm…" duruyor.

**İkonlar çizildi, dizilmedi.** Kenar çubuğu ve killswitch'ler tek tek
unicode karakterleriydi — "▤", "⛨", "◈". Bu, hangi font çözerse ona benzemek
demek: farklı ağırlıklar, farklı optik boyutlar, sembol fontu olmayan
makinede hiç yok, ve hiçbiri Maze markının ince eşit çizgisine uymuyor.
`components/MazeIcon.qml` tek bir 24 birimlik ızgarada, tek çizgi
kalınlığında, yalnızca stroke ile 11 ikon çiziyor (QtQuick.Shapes). Aynı
geometri Android tarafında vector drawable olarak da üretildi, yani iki
uygulama birebir aynı seti kullanıyor.

Sürüm 0.7.0. 13 test süiti geçiyor, derleyici ve qmllint uyarısı yok.


---

## 0.8.0 — bu turda değişenler

**Bir gün sonra iki uygulama artık birbirini bulamıyordu, telefonu tamamen
kapatıp açmak gerekiyordu.** Kök neden: `Connection`'da TLS el sıkışması
bittikten sonra hiçbir taraf peer'in hâlâ orada olduğunu kanıtlamıyordu — ne
TCP keepalive, ne uygulama seviyesinde bir heartbeat. Gerçek bir ağda bağlantı
sessizce yarı açık kalabiliyor (Wi-Fi yeniden ilişkilendirme, DHCP kirası
yenilenmesi, router'ın NAT tablosunu boşaltması) ve FIN/RST hiç gelmiyor;
soket "bağlı" görünmeye devam ediyor. `DeviceManager` da zaten bir `links`
kaydı varsa yeniden çevirmeyi reddediyordu (`links.containsKey` / `m_links`
kontrolü), yani ölü bağlantı sonsuza dek "bağlı" kalıp yenisinin kurulmasını
engelliyordu. Yalnızca telefonu tamamen kapatmak süreci sıfırlayıp
`links`'i temizlediği için "çalışıyordu" — ve masaüstü hiç aramadığı için
(yalnızca telefon arar) bu özellikle telefon tarafında görünüyordu.

Çözüm, `Connection`'ın kendi içine gömülü, `DeviceManager`'ın hiç görmediği
bir Ping/Pong heartbeat: 15 saniye sessizlikte bir Ping gidiyor, 45 saniye
(3 kaçırılan ping) boyunca hiç yanıt gelmezse bağlantı `fail()` ile kapanıyor
— bu da zaten var olan, doğru çalışan yeniden bağlanma döngüsünü tetikliyor.
Yeni `MessageType::Ping`/`Pong`, `Connection::onReadyRead()` içinde
yakalanıp cevaplanıyor/yutuluyor, `messageReceived` hiç emit edilmiyor.
`Limits::kHeartbeatIntervalMs`/`kHeartbeatTimeoutMs` test için
`setHeartbeatIntervals()` ile geçersiz kılınabiliyor (`setHandshakeTimeout()`
ile aynı desen). `tst_transport.cpp`'ye iki test eklendi:
`heartbeatPongsKeepAnIdleLinkAlive` (sağlıklı bağlantı heartbeat trafiğiyle
hayatta kalıyor) ve `heartbeatDropsAnUnresponsivePeer` (yanıt vermeyen ham
bir TLS peer'e karşı timeout gerçekten tetikleniyor — `Server`'ın kendi
`incomingConnection()`'ı gibi ham `QSslSocket` kuran, `readyRead`'e hiç
bağlanmayan bir `UnresponsivePeer` test yardımcısıyla).

Mobil tarafta ayrıca kullanıcının kendi isteği üzerine bir kaçış yolu:
"Bağlı değil" durumundaki eşleşmiş cihazlarda bir **Reconnect** düğmesi —
mevcut linki kapatıp hemen yeniden çeviriyor (`DeviceManager.forceReconnect`).

Sürüm 0.8.0. 13 test süiti geçiyor, uyarı yok.


---

## 0.9.0 — bu turda değişenler

Üç yeni özellik, hepsi mobil tarafın istediği yönde: Android paylaşım
menüsü, telefonda favori komut widget'ı, "telefonda aç".

**Komutlara `pinned` alanı eklendi.** `Command` artık `bool pinned`
taşıyor; `CommandRunner::catalog()`/`writeCatalog()` okuyup yazıyor,
`DeviceManager`'ın `CommandList` cevabı `pinned`'ı telefona da gönderiyor
(salt eklemeli — protokol sürümü sabit). Hangi komutların telefonun
ana ekran widget'ında göründüğü **yalnızca burada**, `CommandsView.qml`'in
yeni "Show on phone widget" düğmesiyle belirleniyor; telefon tarafı sadece
çalıştırabiliyor, pinleyemiyor. Starter dosyasındaki 14 örnekten dördü
(`lock`, `uptime`, `disk`, `mute`) varsayılan olarak pinli — hiçbiri
`confirm` taşıyanlardan değil, çünkü ana ekran düğmesi yanlışlıkla
basılmaya en açık yer.

**Yeni capability: `OpenOnPhone` (`0x20`).** Tek yönlü, bilgisayar →
telefon: panodaki metni/linki gönderiyor. Sistem tepsisine "Send
clipboard to phone" eklendi (`Tray` artık bir `Backend*` alıyor);
`DeviceManager::sendOpenOnPhone()` bağlı ve capability'yi kabul eden ilk
linke gönderiyor — telefon tarafında seçilecek belirli bir cihaz olmadığı
için mobilin kendi "ilk ulaşılabilen" deseniyle aynı mantık. Yeni
`MessageType::OpenOnPhone`, desktop'ın kendi switch'inde asla gerçekten
gelmeyen bir "ignoring unsolicited" dalı (AiModelList/AiChunk/AiDone
grubuyla aynı desen).

`tst_commandrunner.cpp`'ye `pinned` round-trip testleri,
`tst_interop.cpp`'ye `capabilityWireNames()` içinde `OpenOnPhone`
capability + mesaj şekli assertion'ları eklendi.

Sürüm 0.9.0. 13 test süiti geçiyor, uyarı yok.


---

## 1.0.1 — Devices görünümüne Scan butonu

İstenen: mobil ve masaüstü sürümlerin ikisinde de bir tarama butonu.

Keşif pasif çalışıyor, o yüzden boş bir cihaz listesi iki ayrı durumu aynı
anda gösteriyor — ortada gerçekten bir şey yok, ya da biz dinlemeyi
bıraktık — ve kullanıcının ikincisine müdahale etmek için uygulamayı
yeniden başlatmaktan başka yolu yoktu.

- `Beacon::refresh()`: soketi yeniden bind edip grubu her arayüzde yeniden
  join ediyor. Sadece yeniden duyuru göndermek yetmez: bir multicast
  üyeliği join edildiği **arayüze** aittir ve o küme `start()` anında bir
  kez belirlenir. Sonradan kablo takmak, VPN bağlamak veya makineye yeni
  bir adres vermek o arayüzde grubu join etmiş yapmıyor. Yeniden başlatmak
  tek güvenilir çözüm ve maliyeti yok.
- `DeviceManager::rescan()`: beacon'ı tazeliyor, bir sonraki tick yerine
  hemen duyuru gönderiyor, eşleşmiş cihazları yeniden arıyor.
- `Backend::rescanDevices()` slot'u, ve `DevicesView`'a gerçek bir başlık
  satırı (SectionLabel "N devices" + Scan butonu). Buton kalıcı, sadece boş
  durumda değil: bayat bir cihaz gösteren liste de en az boş liste kadar
  taramaya muhtaç ve bu görünümde basılacak başka bir şey yok. Boş durumda
  ayrıca "Scan again" var.

Mobil karşılığı 0.10.3'te (`DeviceManager.rescan()` + `DevicesScreen`).

Sürüm 1.0.1-1. `ctest` 13/13 geçti. QML, offscreen çalıştırmada tek uyarı
üretmedi. `dist/maze-connect-1.0.1-1-x86_64.pkg.tar.zst`.


---

## 1.0.2 — bekleyen eşleştirme link kapanınca temizlenmiyordu

`dropConnection()` `forgetPendingWork()` çağırıyor ama `m_pendingPairings`'e
dokunmuyordu. `requestPairing()` o cihaz haritada dururken baştan
`AlreadyPending` dönüyor, dolayısıyla el sıkışma ortasında kapanan bir
linkin geride bıraktığı kayıt, sonraki her Pair basışını yalnızca bir durum
satırı yazan no-op'a çeviriyor — çağrı hiç denenmiyor, karşı cihazda kesin
olarak hiçbir şey olmuyor. `requestPairingAt()` içindeki
`Connection::failed` yalnızca hiç kurulamayan çağrıyı kapsıyor; **kurulup
sonra düşen** bir el sıkışma (karşı taraf rate-limit uyguladı, kullanıcı
cevaplamadı, ağ takıldı) buradan geçiyor ve slotu süreç ömrü boyunca dolu
bırakıyordu. Mobil taraftaki `_pendingPairing` hatasının (0.10.4) birebir
aynası.

Temiz bir kapanışta da `pairingFailed` yayılıyor, yoksa doğrulama katmanı
gitmiş bir linkin üstünde asılı kalıyor.

**Not:** bu gerçek bir hata ama 23 Ağustos'ta bildirilen "masaüstünden
Pair'e basınca telefon cevap vermiyor" şikâyetinin sebebi değil —
journald logu masaüstünün o sırada takılmadığını, el sıkışmanın
`showing verification code` aşamasına kadar ilerlediğini gösteriyor.
Sebep hâlâ açık; telefon tarafının logcat'ine ihtiyaç var.

Sürüm 1.0.2-1. `ctest` 13/13.


---

## 1.0.3 — arayüz değişince keşif grubuna yeniden katıl

Soru: masaüstünde maze-cloak MAC adresini sürekli değiştiriyor, bu Maze
Connect'e engel olur mu?

Ölçüldü. 17:50:56'da gerçek bir rotasyon oldu (`02:0c:29:…` →
`02:21:5a:…`, kalıcı adres `34:5a:60:63:85:39`) ve hemen ardından:

    IP           : 192.168.0.45  (degismedi)
    /proc/net/igmp: enp42s0 -> 239.255.83.10  (uyelik duruyor)

Yani bu rotasyon hiçbir şeyi bozmadı, çünkü `maze-cloak`'un `set_mac()`'i
önce **link'i indirmeden** canlı değişikliği deniyor ve sürücü kabul etti.
Kiralama da aynı adresi geri verdi.

Ama bu şanslı yol. `set_mac()` canlı değişiklik reddedilirse
`ip link set down` / `up` yoluna düşüyor, ve o yol üyeliği düşürüyor.
Masaüstünün buna karşı **hiçbir savunması yoktu**: ağ değişikliğini izleyen
bir şey yok, `Beacon::refresh()` yalnızca Scan butonundan çağrılıyordu.
Sonuç asimetrik ve dışarıdan teşhisi berbat bir arıza olurdu — giden
duyuru route'u izlediği için telefon bu bilgisayarı görmeye devam eder,
bilgisayarın kendi listesi sessizce boşalır. (Mobilde 0.10.3'te düzelttiğim
hatanın aynısının masaüstü versiyonu.)

Düzeltme: `rejoinIfInterfacesChanged()`, zaten on saniyede bir çalışan
reconnect süpürmesine takıldı. Up/running/non-loopback arayüzlerin
ad+IPv4 imzasını karşılaştırıyor, değiştiyse beacon'ı tazeleyip hemen
duyuru gönderiyor. İmzaya **adresler de dahil**: kiralama değişikliği
arayüz listesini aynı bırakıp üstüne kurulan her şeyi geçersiz kılıyor.
İlk geçişte tetiklenmiyor — `start()` daha yeni join etmişken çalışan bir
soketi boşuna yıkmanın anlamı yok.

Abone olmak yerine yoklamak bilinçli: Qt'nin `QNetworkInformation`'ı
erişilebilirlik bildiriyor, arayüz topolojisini değil; NetworkManager'a
DBus bağımlılığı ise bunu tek bir ağ yığınına bağlardı. Zaten atan bir
timer'da on saniyede bir syscall'ın maliyeti yok.

**Eşleştirme MAC'ten etkilenmiyor**, bu arada: pin `identity.key`'deki EC
anahtarı, MAC ile hiçbir ilgisi yok. Bir rotasyon hiçbir koşulda
eşleştirmeyi bozamaz.

**Firewalld de engel değil** (ölçüldü): `public` zone target=DROP ama
`maze-connect` servisi aktif, 38271/tcp + 38271/udp açık. Kanıt: telefonun
beacon'ı bu firewall'un ardından alındı ve journald'da 15 tane
`link established` var, yani gelen TCP de geçiyor.

Sürüm 1.0.3-1. `ctest` 13/13.


---

## 1.3.0 — Dashboard artık telefonu gösteriyor; telefonu bul; telefondan metin

Bildirilen: masaüstündeki Dashboard bilgisayarın kendi CPU/bellek/diskini
gösteriyordu — kullanıcının zaten önünde oturduğu makinenin okuması. Anlamsız.
KDE Connect'ten daha iyi ve daha profesyonel olması istendi.

**Dashboard = bağlı telefonlar.** Her eşleşmiş telefon için bir kart: pil
halkası (seviye, şarj ve kaynağı, sıcaklık, sağlık), depolama ve bellek
ölçerleri, ağ türü + Wi-Fi çubukları, zil modu, Rahatsız Etmeyin, güç
tasarrufu, model/Android/açık kalma süresi; altında *Çaldır*, *Dosya gönder*,
*Panoyu gönder*. Erişilemeyen telefon son okumasını soluk ve yaşıyla birlikte
tutuyor. Altında "Bu bilgisayar" kutucukları (aktarımlar, komutlar, guard,
adres). Sidebar'da Dashboard artık ilk sayfa (Ctrl+1) ve telefon eşleşmişse
açılış sayfası; eşleşme yoksa Devices açılıyor.

Bilgisayarın kendi anlık görüntüsü kaybolmadı — telefonun panosuna gitmeye
devam ediyor, sadece burada çizilmiyor. `Backend.systemStatus` kaldırıldı.

**Yeni yetenekler** (protokol sürümü değişmedi; yalnızca ilan eden eşe
gönderiliyor, eski eşleşmelere `knownCapabilities` göçüyle otomatik veriliyor):

| Yetenek | Yön | Mesajlar |
| --- | --- | --- |
| `phoneStatus` (0x80) | bilgisayar sorar, telefon cevaplar | `phoneStatusRequest` → `phoneStatus` |
| `findPhone` (0x100) | bilgisayar → telefon | `findPhone` → `findPhoneResult` |
| `shareText` (0x200) | telefon → bilgisayar | `shareText` |

Güvenlik tarafında sabitlenenler:
- `phoneStatus` yalnızca **istenmişse** kabul ediliyor ve
  `phonestatus::sanitize()` her alanı tür/aralık/kapalı listeyle süzüyor;
  geçemeyen alan **yok** sayılıyor, varsayılana düşmüyor (%0 pil sahte alarm
  olurdu).
- `shareText` için `Message::text()` eklendi: tab/LF/CR serbest, diğer kontrol
  karakterleri ve **bidi override/isolate** (U+202A–202E, U+2066–2069) alanı
  tümden reddediyor — bir linki başka yere gidiyormuş gibi gösteren karakterler.
  16 384 karakter üstü reddediliyor (kırpılmıyor), cihaz başına 10 sn'de en
  fazla 5. Metin panoya gidiyor, bildirim + Activity kaydı çıkıyor; http(s)
  linki **yalnızca bildirime tıklanınca** açılıyor, telefon asla açamıyor.
- `findPhoneResult` yalnızca açık bir çaldırma varken alınıyor; `ringing:false`
  sonrası bir sonraki çaldırmaya kadar hiçbir şey alınmıyor. Link kopunca UI
  "Stop ringing" göstermeyi bırakıyor.

**Bildirimler ve tepsi.** Düşük pil (%15, döngü başına bir kez; şarj ya da
%20 üstü yeniden kuruyor), tam şarj (yalnızca 100'e *çıkışta* — yeniden
başlatınca zaten dolu olan telefon için değil). Tepsi menüsüne *Find my phone*,
ipucuna telefonlar ve pil yüzdesi. Pencere tepside gizliyken pano 5 sn'de bir
yoklamıyor (`Window.window.visible` dahil ediliyor), dakikada bir.

**Testler:** yeni `tst_phonelink` (9 vaka) — sahte telefon: bilgisayarın
deposuna anahtarı yazılmış çıplak bir `Connection`, gerçek karşılıklı TLS
üzerinden istenmemiş cevap, sel, bidi hilesi, çaldırmasız sonuç gönderiyor.
"İstenmemiş okuma" kontrolü mutasyonla kapatılınca test düşüyor.
`tst_interop::phoneMessageShape` mobildeki eşiyle aynı adları çiviliyor.
**15/15 ctest**, qmllint uyarısız (önceden kalan `CommandsView` ve
`GlassPanel` uyarıları da giderildi).

Görsel doğrulama: `PhoneCard` sahte veriyle ekransız çizilip ekran görüntüsü
alındı (bağlı/şarjda, erişilemeyen/düşük pil, eski sürüm telefon). Gerçek bir
telefonla canlı tur yapılmadı — mobil 0.15.0 ile birlikte denenecek.

Sürüm 1.3.0-1. **Mobil 0.15.0 ile birlikte yayınla**; eski mobil sürüm bu
yetenekleri ilan etmediği için kart "update it to 0.15.0" diyor.
