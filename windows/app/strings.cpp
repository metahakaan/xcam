#include "app/strings.h"

#include <windows.h>

#include <unordered_map>

namespace xcam {
namespace {

Lang g_lang = Lang::English;

// Turkish, keyed by the English. Only strings that appear on screen are here;
// log lines stay English because they are read alongside code.
//
// Camera vocabulary is left alone on purpose. ISO, shutter, LUT and log are the
// words on every camera body sold in Turkey, and translating them would make
// the panel harder to read for exactly the people who know what they mean.
const std::unordered_map<std::string, const char*>& TurkishTable() {
    static const std::unordered_map<std::string, const char*> table = {
        // ---- states ---------------------------------------------------------
        {"connecting", "bağlanıyor"},
        {"waiting for a phone on adb", "adb üzerinde telefon bekleniyor"},
        {"looking for a phone on the network", "ağda telefon aranıyor"},
        {"looking for a phone on USB or the network", "USB veya ağda telefon aranıyor"},
        {"starting the phone app", "telefon uygulaması başlatılıyor"},
        {"handshake failed", "el sıkışma başarısız"},

        // ---- format readouts ------------------------------------------------
        {"SIZE", "BOYUT"},
        {"RATE", "HIZ"},
        {"CODEC", "KODEK"},
        {"BITRATE", "BİTHIZI"},

        // ---- the pro column -------------------------------------------------
        {"SHUTTER", "ENSTANTANE"},
        {"WB", "BEYAZ"},
        {"FOCUS", "ODAK"},
        {"RESET AUTO", "OTOMATİĞE DÖN"},
        {"AUTO", "OTO"},
        {"INF", "SONSUZ"},

        // ---- the device tray ------------------------------------------------
        //
        // The headings say what a row of buttons is for. They exist because the
        // row underneath them used to be seven unrelated chips, and nothing on
        // screen said which of them mattered while a call was running.
        {"CAMERAS", "KAMERALAR"},
        {"EXPOSURE", "POZ"},
        {"COLOUR", "RENK"},
        {"SHAPE", "BİÇİM"},
        {"PROMPTER", "PROMPTER"},
        {"LOAD A SCRIPT", "METİN YÜKLE"},
        {"SHOW", "GÖSTER"},
        {"HIDE", "GİZLE"},
        {"PLAY", "OYNAT"},
        {"PAUSE", "DURAKLAT"},
        {"TOP", "BAŞA"},
        {"MIRROR", "AYNA"},
        {"a plain text file, one you can read aloud",
         "düz bir metin dosyası, sesli okuyabileceğin"},
        {"only you see this -- not the call, not the recording",
         "bunu yalnızca sen görürsün — görüşme de kayıt da görmez"},
        {"WIDE", "GENİŞ"},
        {"VERTICAL", "DİKEY"},
        {"SQUARE", "KARE"},
        {"reconnect the camera in the app that is using it",
         "kamerayı kullanan uygulamada yeniden bağla"},
        {"BRIGHT", "PARLAKLIK"},
        {"CONTRAST", "KONTRAST"},
        {"SATURATION", "DOYGUNLUK"},
        {"WARMTH", "SICAKLIK"},
        {"RESET COLOUR", "RENGİ SIFIRLA"},
        {"the picture is as the camera sent it", "görüntü kameranın gönderdiği gibi"},
        {"this reaches the call and the recording", "bu, görüşmeye ve kayda da işler"},
        {"FORMAT", "FORMAT"},
        {"changing these restarts the stream", "bunları değiştirmek yayını yeniden başlatır"},
        {"pre-roll", "ön-tampon"},
        {"RECORDING", "KAYIT"},
        {"LOOK", "GÖRÜNÜM"},
        {"PRO", "PRO"},

        // "WEBCAM" rather than "CAM": the old label could as easily have meant
        // "choose a camera", and this button decides whether anybody else can
        // see you.
        {"WEBCAM", "WEBCAM"},
        {"MIC", "MİKROFON"},
        {"TORCH", "FENER"},
        {"RECORD", "KAYIT"},
        {"OFF", "KAPALI"},
        {"MAX", "EN İYİ"},
        {"TO PC", "PC'YE"},
        {"TO PHONE", "TELEFONA"},

        // ---- stats ----------------------------------------------------------
        {"dropped", "kare düştü"},
        {"free", "boş"},
        {"link limited to", "bağlantı sınırı"},

        // ---- settings -------------------------------------------------------
        {"Settings", "Ayarlar"},
        {"Language", "Dil"},
        {"Connection", "Bağlantı"},
        {"Automatic", "Otomatik"},
        {"USB only", "Yalnızca USB"},
        {"Wi-Fi only", "Yalnızca Wi-Fi"},
        {"Phone address", "Telefon adresi"},
        {"Type an address, Enter to save", "Adres yazın, kaydetmek için Enter"},
        {"Recordings folder", "Kayıt klasörü"},
        {"Choose…", "Seç…"},
        {"Presets", "Hazır ayarlar"},
        {"Save current", "Bunu kaydet"},
        {"Empty", "Boş"},
        {"Record every frame", "Her kareyi kaydet"},
        {"Record interval", "Kayıt aralığı"},
        {"every frame", "her kare"},
        {"every 2nd frame", "2 karede bir"},
        {"every 4th frame", "4 karede bir"},
        {"every 8th frame", "8 karede bir"},
        {"Apply the look to recordings", "Görünümü kayda uygula"},
        {"OFF", "KAPALI"},
        {"Takes on the phone only", "Yalnızca telefona kayıtta"},
        {"Takes begin %ds before you press record", "Çekim, kayda basmandan %d saniye önce başlar"},
        {"The encoder runs the whole time this is armed", "Kurulu olduğu sürece kodlayıcı çalışır"},
        {"This phone is too old for it", "Bu telefon bunun için eski"},
        {"Not enough memory on the phone", "Telefonun belleği yetmiyor"},
        {"The take starts when you press it", "Çekim bastığın anda başlar"},
        {"Takes on the phone", "Telefondaki çekimler"},
        {"microphone silent", "mikrofon sessiz"},
        {"Start with Windows", "Windows ile başlat"},
        {"On", "Açık"},
        {"Off", "Kapalı"},
        {"Recordings", "Kayıtlar"},
        {"Application", "Uygulama"},
        {"the phone can only give the recording", "telefon kayda yalnızca şunu verebiliyor:"},
        {"FRAME", "KADRAJ"},
        {"Zebras and focus peaking, on the preview only", "Zebra ve odak zirvesi, yalnızca önizlemede"},
        {"this PC", "bu PC"},
        {"phone", "telefon"},
        {"Sound", "Ses"},
        {"Microphone on this PC", "Bu PC'deki mikrofon"},
        {"Input", "Giriş"},
        {"Default", "Varsayılan"},
        {"Recorded as a second track beside the phone's", "Telefonunkinin yanına ikinci iz olarak kaydedilir"},
        {"Mirror", "Ayna"},
        {"Upside down", "Baş aşağı"},
        {"Refresh", "Yenile"},
        {"Stop", "Durdur"},
        {"Delete", "Sil"},
        {"Keep", "Vazgeç"},
        {"Delete %s? It cannot be undone", "%s silinsin mi? Geri alınamaz"},
        {"No phone connected", "Telefon bağlı değil"},
        {"Asking the phone…", "Telefona soruluyor…"},
        {"This phone cannot record", "Bu telefon kayıt alamıyor"},
        {"Nothing recorded on the phone yet", "Telefonda henüz kayıt yok"},
        {"Browse", "Göz at"},
        {"Look", "Görünüm"},
        {"No matte", "Kaşe yok"},
        {"rows masked top and bottom", "satır altta ve üstte maskeli"},
        {"A second file, re-encoded here from the stream", "Akıştan bu PC'de yeniden kodlanan ikinci bir dosya"},
        {"Close", "Kapat"},
        {"Nothing saved yet", "Henüz kayıt yok"},
        {"Click a slot to load, right-click to replace",
         "Yüklemek için tıklayın, değiştirmek için sağ tıklayın"},
    };
    return table;
}

}  // namespace

void Strings::Init() {
    // The Turkish locale ids all begin 0x1F; the sublanguage does not matter.
    const LANGID id = GetUserDefaultUILanguage();
    g_lang = (PRIMARYLANGID(id) == LANG_TURKISH) ? Lang::Turkish : Lang::English;
}

void Strings::Set(Lang lang) { g_lang = lang; }

Lang Strings::Current() { return g_lang; }

const char* Strings::Name(Lang lang) {
    return lang == Lang::Turkish ? "Türkçe" : "English";
}

const char* Strings::Get(const char* english) {
    if (g_lang == Lang::English || !english) return english;

    const auto& table = TurkishTable();
    const auto it = table.find(english);
    return it == table.end() ? english : it->second;
}

}  // namespace xcam
