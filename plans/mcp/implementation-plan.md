# HDMapping MCP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Udostępnić pełne przetwarzanie kroków 1–3 przez lokalne MCP oraz objąć ciężkie zadania GUI i MCP wspólnym trwałym koordynatorem.

**Architecture:** Adapter MCP stdio i koordynator w Go; wspólne operacje, mosty GUI i izolowane procesy zadań w C++. GUI jest właścicielem otwartego stanu, zadania pracują na utrwalonych snapshotach. Wyniki są publikowane osobno i stosowane do właściwego GUI przy zgodnej rewizji.

**Tech Stack:** C++20, CMake >= 4.0, obecne biblioteki HDMapping, Go i oficjalne `modelcontextprotocol/go-sdk`. SDK v1.7.0 jest zweryfikowanym punktem odniesienia dla obsługi MCP 2026-07-28 i wymaga Go >= 1.25.0; dokładny patch toolchainu i SDK kwalifikuje P00, a P01 przypina je wraz z zależnościami.

**Spec:** [design.md](design.md); decyzje Q1–Q20: [design-tree.md](design-tree.md); źródła: [findings.md](findings.md).

Status: plan zaakceptowany do pracy po jednej karcie w osobnym czacie. Żadna karta nie została jeszcze wykonana. Właściciel forka zlecił wersjonowanie tego pakietu w `plans/mcp/`. Oznaczenia P00–P17 są kartami planu, nie numerami zgłoszeń GitHub. Zasady wykonania, dostarczenia i następnego promptu: [README.md](README.md); bieżący stan: [status.md](status.md).

## Global Constraints

- C++20; CMake >= 4.0; Windows 11, macOS i Ubuntu 24.04.
- Jedno repozytorium, wersja z top-level CMake i wspólne wydanie produktu.
- Domyślny limit: 2 ciężkie zadania; konfigurowalny, z kolejką. Jedna mutacja danego stanu naraz.
- Cały ciąg 1–3 w GUI i headless; ciężkie przyciski GUI używają tego samego koordynatora co MCP.
- Zadania przeżywają zwykłe zamknięcie klienta i okna, także podczas `preparing`; awaria pozwala ponowić od początku, nie od iteracji.
- Wspólny niezapisany stan GUI, jawne zakresy, kontrola rewizji; osobne wykrywanie/dodanie/zastąpienie więzów.
- Osobne wyniki domyślnie; istniejące niezmienne chmury mogą być zależnościami; pełny eksport na żądanie.
- Istniejące formaty, opcje CLI, jednostki, parametry i wyniki obliczeń zachowują znaczenie.
- Jedna odpowiedzialność na PR, bez porządkowania pobocznego kodu. Każda karta ma własny odbiór; jeśli diff wymaga podziału, dzielić w obrębie tej karty, zachowując jej kryteria.
- Build z MCP dostarcza Go automatycznie z przypiętych oficjalnych archiwów lub używa zgodnego systemowego kompilatora; nie wymaga ręcznych poleceń Go. Gotowy pakiet nie wymaga Go/Python/Node u użytkownika.
- Opcja `BUILD_WITH_MCP`: OFF podczas dostarczania niekompletnych kart, ON w kompletnym wydaniu po P17. OFF nie wymaga Go i zachowuje dotychczasowy model GUI. To techniczna propozycja autora, zgodna z jednym wydaniem produktu.
- Dane odbiorowe poza repozytorium. Testy logiki i kontraktów; GUI sprawdzane w rzeczywistej aplikacji, szczególnie na Windows.
- Wykonuj wyłącznie kartę wskazaną w bieżącym prompcie użytkownika. Dostarczenie na fork i następny prompt według README; merge i release wymagają odrębnego zlecenia.

## Review Focus

1. **Zamknięcie okna w `preparing` z chmurą tylko w RAM:** ID zadania pozostaje ważne, przekazanie danych kończy się w tle — P04/P06.
2. **Dwa okna tego samego pliku i dwa klienty AI:** rewizje, identyfikatory i blokady nie mieszają stanów ani zapisów — P02/P05/P06.
3. **Ukryty skan oraz niezapisane CP/GCP/więzy:** jawny zakres i snapshot odtwarzają dokładne wejście zadania — P04/P09/P10/P12.
4. **Awaria między zapisem poses a publikacją projektu:** poprzedni wynik i źródła pozostają spójne, częściowy nowy wynik nie jest sukcesem — P05/P14.
5. **Paczka przeniesiona poza build, ścieżki ze spacjami i polskimi znakami:** odnajduje worker, biblioteki, słownik BoW i dane bez source tree — P01/P03/P11/P17.

## Podział plików i interfejsów

Poniższe ścieżki i nazwy są propozycją autora planu. Przed dodaniem każdego typu ponownie wyszukać odpowiednik w `core/` i `shared/`; znaleziony odpowiednik ma pierwszeństwo. Nowe klasy C++ mają własne `.h`/`.cpp` i krótkie `//!` na deklaracjach.

| Miejsce | Odpowiedzialność |
| --- | --- |
| `apps/hdmapping_mcp/` | Jeden moduł Go; `cmd/mcp/main.go`, `cmd/coordinator/main.go`, pakiety `internal/{protocol,jobs,supervisor,mcp}`. |
| `apps/hdmapping_worker/` | C++ executable do wykonywania operacji z utrwalonego wejścia; bez GUI i modelu AI. |
| `processing/include/HDMapping/Processing/` i `processing/src/` | Kontrakty operacji, snapshoty, publikacja, katalog operacji i współdzielony most GUI. |
| `apps/lidar_odometry_step_1/step1_operations.h/.cpp` | Adapter operacji kroku 1 do istniejących obliczeń. |
| `apps/multi_view_tls_registration/step2_operations.h/.cpp` | Adapter operacji kroku 2; odpowiedzialności więzów/georeferencji/segmentów w osobnych parach plików. |
| `apps/multi_session_registration/project_io.h/.cpp`, `step3_operations.h/.cpp` | Format i stan projektu oraz operacje kroku 3. |
| `processing/tests/` | Doctest oraz testy kontraktów procesu; bez odtwarzania rysowania GUI. |
| `cmake/go.cmake`, istniejące CMake/install/CI/deploy | Budowanie Go w tym samym grafie CMake i pakowanie całego produktu. |

Nie przenosić całych aplikacji do nowej biblioteki. `processing_ops` zawiera kod niezależny od `WITH_GUI`; końcowy proces GUI używa `core`, a worker `core_no_gui`. Nie linkować obu wariantów core do jednego executable. Zachować współdzielone `core_math`; nie powielać ciężkich źródeł optymalizacji. Krok 1 ma już wspólny `step1_run_report` — użyć go.

### Kontrakt między kartami

Definiowany w P01/P03, obowiązuje dalsze karty:

- `TargetRef { state_id: string, revision: uint64 }` — identyfikator otwarcia, nie ścieżka.
- `OperationRequest { operation: string, target: TargetRef, scope: object, parameters: object }` — schemat każdego `scope/parameters` pochodzi z rejestru C++.
- `SnapshotRef { manifest_path: filesystem::path, target: TargetRef }` — manifest opisuje rodzaj danych, wszystkie zależności i ich niezmienność/czas życia.
- `RunContext { output_directory: filesystem::path, stop: std::stop_token, progress: ProgressSink }`.
- C++ `RunResult = std::variant<ArtifactManifest, OperationError>`; `OperationError` ma kod, komunikat i szczegóły. `ProgressEvent` ma etap, opcjonalne liczniki i komunikat, bez obowiązkowego procentu; `ProgressSink = std::function<void(const ProgressEvent&)>`.
- `ArtifactManifest` zawiera identyfikator wyniku, wejściowy `TargetRef`, wynikowy `SnapshotRef`, własne pliki i zależności zewnętrzne. `OperationDescriptor` zawiera ID, schematy argumentów/wyniku i klasę `read/mutate/heavy`.
- C++ `DescribeOperations() -> std::vector<OperationDescriptor>` oraz `ExecuteOperation(const OperationRequest&, const SnapshotRef&, RunContext&) -> RunResult` dla operacji na snapshotach.
- Adapter aktualnego stanu udostępnia `DescribeState() -> StateSummary` i `ApplyMutation(const OperationRequest&) -> MutationResult`, gdzie `StateSummary` zawiera cel, parametry i zwięzłe metadane, a `MutationResult = std::variant<StateSummary,OperationError>`. Krótka zmiana parametrów nie wymaga kopiowania chmur; używa tej samej walidacji i funkcji zmiany C++ co stan headless.
- Go: `Submit(ctx context.Context, req SubmitRequest) (JobRecord, error)`, `Get(ctx context.Context, id JobID) (JobRecord, error)`, `List(ctx context.Context, filter JobFilter) ([]JobRecord, error)`, `Cancel(ctx context.Context, id JobID) (JobRecord, error)`. `JobID` jest typem opartym na string, `JobFilter` filtruje po stanie/statusie z limitem i kursorem. `SubmitRequest` zawiera `request_key`, `OperationRequest`, źródło GUI/MCP i opis snapshotu/dojścia do źródła.
- `JobRecord` zawiera `job_id`, `request_key`, stan, cel/revizję, operację, snapshot, postęp, katalog wyniku, błąd oraz osobny `gui_apply_status`.
- RPC GUI: `states.list`, `states.describe`, `states.execute`, `states.capture`, `states.apply_result`; operacje muszą docierać na wątek GUI. `execute` obsługuje krótkie mutacje z rewizją; ciężkie operacje trafiają do `Submit`. `capture` rozpoczyna przekazanie z rezerwacją danych, a zakończenie zwraca `SnapshotRef`.
- Nazwy docelowych narzędzi MCP są dziedzinowe, np. `run_odometry`, `register_scans`, `optimize_project`, `export_result`, `get_job`, `cancel_job`. To nie publiczny odpowiednik ogólnego shell/execute-code.

P01/P03 zapisują kontrakty w kodzie i testach. Prywatne wire schema ma numer wersji; nie staje się nowym formatem plików sesji.

## Kolejność i zależności

`P00 → P01 → P02 → P03 → P04 → P05 → P06 → P07`.

Po tym: `P08 → P09 → P10 → P11 → P12 → P13` oraz `P14 → P15 → P16`; P14 korzysta z kontraktów snapshotu, publikacji i więzów wypracowanych wcześniej. **P17 wymaga wszystkich kart.** Zależności można wykorzystać do równoległego rozpoznania/review; współdzielonych plików i kontraktów nie modyfikować niezależnie w konkurujących gałęziach.

Każda karta kończy się sprawdzeniami, odczytem `git diff --submodule`, przeglądem oraz commitem i pushem na branch tej karty na forku, jeżeli bieżący prompt zleca jej wykonanie według README. Otwarcie PR wymaga osobnego zlecenia. Nie powtarzać pełnych kosztownych testów bez nowej przyczyny; zachować dowody dla dokładnego SHA.

## P00 — Kwalifikacja protokołu, procesów i danych

**Pliki:** tymczasowy prototyp i duże dowody poza śledzonym drzewem; trwały, zwięzły raport w `plans/mcp/results/P00.md`, aktualizacja `status.md` i prompt kontynuacji/następnej karty. Raport zawiera odtwarzalne komendy, wersje, checksums i wyniki; nie zależy wyłącznie od plików na jednej maszynie. Bez kodu produktu i trwałego narzędzia jednorazowego.
**Interfejs:** dostarcza wybrane wersje/toolchain/IPC i wyniki prób do P01–P03; nie zmienia zakresu Q1–Q20.

- [ ] Zbudować minimalny serwer oficjalnego Go SDK i sprawdzić stdio w rzeczywistym Codex oraz Claude Desktop: discovery, schema, wywołanie i błąd; zapisać negocjowane wersje, nie zgadywać po nazwie aplikacji.
- [ ] Sprawdzić uruchomienie odłączonego koordynatora i kontrolowanego workera po zamknięciu klienta oraz okna na trzech platformach; na Windows sprawdzić także ograniczenia procesów potomnych. Dowodem jest dalsza praca, ponowne połączenie i odczyt tego samego ID.
- [ ] Wybrać mechanizm IPC dostępny wyłącznie dla tego użytkownika oraz metodę uruchamiania na żądanie, bez wymagania uprawnień administratora. Jeżeli jeden mechanizm nie spełnia platform, dostarczyć implementacje platformowe tego samego kontraktu.
- [ ] Przypiąć Go i moduły z oficjalnych źródeł, ustalić checksums toolchainu dla obsługiwanych architektur; wynik staje się wejściem P01. Go SDK v1.7.0 wymaga `go 1.25.0`; nie zakładać, że domyślny pakiet Ubuntu 24.04 wystarczy.
- [ ] Przygotować bazowe wyniki realnego Parking i wybranego nakładającego się podzbioru wielu sesji (kandydat OmniWarsaw). Zapisać konfigurację i natywny wynik; nie uznawać 25 chunków Parking za 25 sesji.
- [ ] Sporządzić macierz istniejących operacji i miejsc mutacji GUI dla kroków 1–3, łącznie z importami/eksportami i zależnościami typu BoW/geoidy. To lista odbiorowa kolejnych kart.

**Brama:** przy niepowodzeniu któregokolwiek warunku technicznego potrzebna korekta projektu, nie założenie, że problem rozwiąże się później. Nie zaczynać szerokiej integracji GUI bez dowodu życia procesów.

## P01 — Budowanie i minimalny MCP produktu

**Pliki:** nowe `apps/hdmapping_mcp/{CMakeLists.txt,go.mod,go.sum,cmd/mcp/main.go,internal/mcp/server.go,internal/mcp/server_test.go}`, `cmake/go.cmake`; istniejące top-level CMake, `cmake/install_app.cmake`, platformowe CI, `deploy_mandeye.bat`, `macos_bundle_toolkit/`; build docs wiki.
**Interfejs:** MCP stdio `server_info`/capabilities z wersją produktu; brak reklamowania niezaimplementowanych operacji.

- [ ] Dodać test klientem SDK: `tools/list` ma poprawne schematy, wersja odpowiada top-level CMake, błędne argumenty dają błąd, log nie trafia na stdout protokołu. Uruchomić i potwierdzić brak wymaganej funkcji przed implementacją.
- [ ] Zintegrować `go build` i testy w CMake, przypięte źródła i moduły. Automatyczny toolchain pobiera wyłącznie oficjalne archiwum z SHA256; systemowy jest używany tylko po sprawdzeniu zgodności. OFF nie szuka/pobiera Go.
- [ ] Dodać cele i `hdmapping_install_app`; rozszerzyć wspólny helper o instalację executable z Go, ponieważ obecne `install(TARGETS)` nie obsłuży wprost imported executable. Zachować komponent `applications`, `DESTDIR`, `.exe` i `HDMAPPING_BINARY_PREFIX`; Windows `EXPECTED` uwzględnia opcję ON/OFF. Nie tworzyć osobnego procesu pakowania.
- [ ] Sprawdzić build/install ON i OFF, `go test ./...` oraz uruchomienie z katalogu instalacji poza buildem, ze spacją i polskim znakiem w ścieżce. PASS: handshake i wynik po przeniesieniu instalacji; bez Go na maszynie użytkownika.

## P02 — Trwały rejestr, idempotencja i kolejka

**Pliki:** `apps/hdmapping_mcp/internal/jobs/{types.go,store.go,store_test.go,scheduler.go,scheduler_test.go}`.
**Interfejs:** `Submit/Get/List/Cancel`, `JobRecord`, rezerwacja `TargetRef`, limit domyślny 2; wykonawca procesu jest wstrzykiwaną zależnością do P03.

- [ ] Test `TestLimitTwoAndReservation`: trzy niezależne zadania → 2 running + 1 queued; druga mutacja tego samego stanu odrzucona; odczyt nadal dostępny.
- [ ] Test `TestRetryAndCrashRecovery`: ten sam request_key + argumenty → to samo ID; inne argumenty → błąd; przerwany zapis rejestru nie tworzy „completed”; obniżenie limitu nie zabija uruchomionych zadań.
- [ ] Zaimplementować rejestr odporny na przerwany zapis, przejścia i rezerwacje; stan sukcesu zapisywać dopiero po potwierdzeniu publikacji. Trwały zapis ma korzystać z mechanizmów zapewniających wymagane zachowanie na trzech platformach, nie samego flush bufora języka.
- [ ] Uruchomić `go test ./internal/jobs/...` także z testami przerwania/restartu; testy nie korzystają z czekania na przypadkowy czas, tylko sterowanego wykonawcy. PASS: wszystkie inwarianty przy mieszanych źródłach GUI/MCP.

## P03 — Niezależny koordynator i proces C++

**Pliki:** `cmd/coordinator/main.go`, `internal/protocol/{messages.go,client.go,server.go}`, `internal/supervisor/` z implementacjami platformowymi i testami; `apps/hdmapping_worker/{CMakeLists.txt,main.cpp}`; `processing/{CMakeLists.txt,include/HDMapping/Processing/Operation.h,src/Operation.cpp,include/HDMapping/Processing/RuntimeProtocol.h,src/RuntimeProtocol.cpp}`; istniejące CMake/install/packaging.
**Interfejs:** kontrakty z wcześniejszej sekcji; worker opisuje operacje i wykonuje wskazane zadanie, raportuje etap i odbiera anulowanie.

- [ ] Dodać integracyjny test procesu: dwa adaptery odnajdują jeden koordynator; zamknięcie adaptera nie kończy fake workera; ponowne połączenie zwraca to samo ID. Fałszywa tożsamość procesu i niezgodna wersja produktu są odrzucane.
- [ ] Zaimplementować mechanizm z P00, nadzór i ponowne odnajdowanie rzeczywiście własnych procesów; nie ufać samemu PID. Registry GUI/worker nie pozwala podszyć się pod innego użytkownika. Odnajdywanie executable obsługuje zarówno nieprefiksowane nazwy z build/bundle, jak i prefiks instalacji `HDMAPPING_BINARY_PREFIX`.
- [ ] Dodać prawdziwy worker C++ z `DescribeOperations` i jawnie nieobsługiwanym nieznanym poleceniem. Wymusić `core_no_gui` przy ON i brak inicjalizacji okna.
- [ ] Przetestować grace cancel, wymuszone zakończenie procesu po timeout oraz awarię koordynatora. Niezakończone/utracone zadanie nie jest automatycznie uruchamiane od nowa; zakończony worker ma trwały wynik możliwy do uzgodnienia z rejestrem.
- [ ] Uruchomić Go integration tests i CTest etykiety `processing-runtime`; powtórzyć próbę procesu w gotowym pakiecie na trzech platformach.

## P04 — Snapshot niezapisanego stanu i zależności

**Pliki:** `processing/include/HDMapping/Processing/SnapshotStore.h`, `processing/src/SnapshotStore.cpp`, `processing/tests/snapshot_store_test.cpp`; minimalne adaptery serializacji stanów w aplikacjach; istniejące `SessionPaths.h`, session/point-cloud IO i cache kroku 1.
**Interfejs:** `CaptureSnapshot(CaptureSource&, std::uint64_t expected_revision, const std::filesystem::path& destination, RunContext&) -> std::variant<SnapshotRef,OperationError>`; `ValidateSnapshot(const SnapshotRef&) -> std::optional<OperationError>`. `CaptureSource` to zdefiniowany w `SnapshotStore.h` adapter domenowego stanu, utrzymujący rezerwację danych do końca przekazania i potrafiący zapisać metadane oraz zmaterializować jego zależności; nie jest kopią layoutu okna. Wczytanie danych przez konkretne operacje używa obecnych loaderów po walidacji snapshotu.

- [ ] Dodać test snapshotu zawierającego niezapisane poses, więzy, parametry, CP/GCP, chmurę tylko w RAM i zależność z cache. Po zamknięciu źródła odczyt daje te same dane.
- [ ] Dodać test źródła zmienionego w trakcie przekazania oraz anulowania; niepełny manifest nie jest wejściem `queued`. Współdzielona zależność nie zostaje usunięta przez cleanup innego zadania.
- [ ] Zaimplementować materializację, katalog zależności i reguły niezmienności/czasu życia. Używać istniejących loaderów/writerów; surowe dane wejściowe pozostają zachowane. Hardlink nie zastępuje kontraktu niezmienności.
- [ ] Przetestować CTest `processing-snapshot` i realny snapshot z GUI; zapisać koszt czasu/dysku/pamięci. Nie tworzyć ukrytej kopii całego archiwum, jeżeli można bezpiecznie współdzielić niezmienne dane.

## P05 — Spójne publikowanie i eksport pełnego wyniku

**Pliki:** `processing/include/HDMapping/Processing/ResultPublisher.h`, `processing/src/ResultPublisher.cpp`, `processing/tests/result_publisher_test.cpp`; `core/src/session.cpp` tylko niezbędne operacje ścieżek; projekt kroku 3 wykorzysta interfejs w P14.
**Interfejs:** `PublishResult(const std::filesystem::path& staging_directory, const std::filesystem::path& destination, const PublishOptions&) -> RunResult`; `PublishOptions` ma jawne replace oraz oczekiwaną tożsamość starego celu. `ExportComplete(const ArtifactManifest&, const std::filesystem::path& destination) -> RunResult` tworzy samowystarczalny zestaw.

- [ ] Testy: konflikt istniejącego celu, dwa zadania na ten sam cel, przerwanie między poses a manifestem, brak miejsca, błąd rename/publikacji; wcześniejszy wynik i źródła pozostają niezmienione.
- [ ] Zaimplementować budowę w staging na woluminie celu, walidację odnośników, natywny odczyt i publikację dopiero po sukcesie całości. Nadpisanie nie może częściowo przestawić starego grafu plików.
- [ ] Dodać test zwykłego wyniku ze współdzieloną chmurą i test pełnego eksportu otwieranego po odłączeniu katalogu źródłowego; zachować jednostki, układy i metadane.
- [ ] Uruchomić CTest `processing-publication` oraz odczyt natywnym loaderem po zakończeniu procesu tworzącego. PASS obejmuje także wyczyszczenie nieużywanego staging bez usunięcia współdzielonych danych.

## P06 — Most GUI, rewizje i bezpieczne zamknięcie

**Pliki:** `processing/include/HDMapping/Processing/GuiStateBridge.h`, `processing/src/GuiStateBridge.cpp`, `processing/tests/state_revision_test.cpp`; punktowe adaptery i pętle w `lidar_odometry_gui.cpp`, `multi_view_tls_registration_gui.cpp`, `multi_session_registration.cpp`.
**Interfejs:** RPC `states.list/describe/execute/capture/apply_result`, adapter `DescribeState/ApplyMutation`; mutacje z `expected_revision`; most z callbackami wywoływanymi na wątku GUI, wyniki z P04/P05.

- [ ] Dodać test logiki rewizji: dwa otwarcia jednej ścieżki mają różne ID; Save As nie zmienia tożsamości; kamera nie zmienia rewizji; mutacja dziecka projektu unieważnia jego wejście.
- [ ] Dodać test zamknięcia źródła w `preparing`: po przyznaniu ID okno znika, proces kończy przekazanie w tle, snapshot pozostaje poprawny. Force-kill źródła daje `interrupted`, nie sukces.
- [ ] Wprowadzić obsługę komend i rezerwacji na wątku GUI, w tym wszystkich istniejących miejsc mutacji z macierzy P00. Ustalenia NDT/ICP kontrolek oraz silnika aktualizują się razem.
- [ ] Zaimplementować auto-apply tylko do nadal otwartego ID i zgodnej rewizji; brak okna lub konflikt zachowuje wynik do późniejszego otwarcia. Nie dopasowywać celu po tytule/ścieżce.
- [ ] Sprawdzić logikę CTest `processing-state` oraz ręcznie trzy GUI na Windows: MCP zmienia parametry, kolejna klatka ich nie cofa, blokada dotyczy danych, kamera działa. Nie pisać testów układu/renderowania.

## P07 — Krok 1 jako wspólne operacje i zadania

**Pliki:** `apps/lidar_odometry_step_1/step1_operations.h/.cpp`, istniejące `lidar_odometry.{h,cpp}`, `lidar_odometry_gui.cpp`, `toml_io.*`, `run_report.*`, CMake; `processing/tests/step1_operations_test.cpp`; katalog MCP w Go.
**Interfejs:** rejestr operacji `step1.load/prepare/odometry/consistency/reference/save/export` korzystający z `ExecuteOperation`; jeden zestaw parametrów/domysłów po stronie C++.

- [ ] Test kontraktu TOML/defaults i błędów wejścia; niewłaściwe/niekompletne dane zwracają `OperationError`, nie sam tekst na stdout.
- [ ] Wydzielić minimalne wywołania istniejących obliczeń; dodać postęp i punkty zatrzymania bez zmiany algorytmów. Istniejące CLI zachowuje argumenty, wyniki i kody błędów.
- [ ] Podłączyć ciężkie przyciski GUI i MCP do `Submit`; przy ON usunąć obejście do lokalnego niezarządzanego workera, zachowując dotychczasową ścieżkę za OFF. Zastąpić porzucenie przez `_Exit` kontraktem anulowania zadania.
- [ ] Uruchomić Parking przez stary kontrakt CLI, wspólną operację headless i przycisk GUI: porównać poses/chmury/raport przy tych samych parametrach i sprawdzić zamknięcie okna. Sprawdzić także referencje i consistency z macierzy.
- [ ] CTest `processing-step1`, rzeczywisty odczyt zapisanej sesji, potem przegląd i dostarczenie tylko tej integracji według README. Otwarcie PR następuje na odrębne zlecenie.

## P08 — Rejestracja skanów kroku 2

**Pliki:** `step2_operations.h/.cpp`, `multi_view_tls_registration.cpp`, `multi_view_tls_registration_gui.cpp`, CMake; istniejące core ICP/NDT/plane features; `processing/tests/step2_registration_test.cpp`.
**Interfejs:** `step2.register_scans` z jawnie wskazanymi skanami i wariantem algorytmu; `OperationDescriptor` publikuje dozwolone parametry i jednostki.

- [ ] Dodać testy błędnego zakresu, parametrów i odrzucenia starej rewizji; rejestracja nie zależy od `pc.visible` poza jawnym rozwiązaniem zakresu.
- [ ] Udostępnić istniejące NDT/ICP/plane features przez wspólne operacje; GUI przyciski wywołują zadania, renderowanie pozostaje w procesie GUI. Nie zmieniać równań, metryk ani domyślnych parametrów.
- [ ] Sprawdzić realną sesję z Parking wobec bazowego wykonania GUI; dodać meaningful regression na znanej geometrii dla przesunięcia/obrotu i utrzymania nieruchomego skanu.
- [ ] CTest `processing-step2-registration`, ręczny odczyt/auto-apply w GUI i dwa równoległe niezależne zadania.

## P09 — Ręczne więzy i optymalizacja grafu jednej sesji

**Pliki:** `step2_constraint_operations.h/.cpp`, istniejące `pose_graph_loop_closure.*`, `manual_pose_graph_loop_closure.*` i GUI; `processing/tests/constraint_operations_test.cpp`.
**Interfejs:** `constraints.list/add/update/remove/replace`, `constraints.run_icp`, `step2.optimize_graph`; aktywny więz odrębny od kandydata.

- [ ] Testy: dodanie nie usuwa starych więzów; replace wymaga jawnego zakresu/revizji; błędny endpoint lub niezgodna sesja nie modyfikuje grafu; zapis/odczyt zachowuje sigmy i transformacje.
- [ ] Wydzielić operacje spod obsługi kliknięć, zachowując istniejące struktury grafu; wybrany i wszystkie wskazane więzy działają przez te same handlery w GUI i workerze.
- [ ] Porównać optymalizację na bazowej rzeczywistej sesji, sprawdzić pause/cancel jako kontrakt zadania, nie zamknięcie procesu GUI.
- [ ] CTest `processing-constraints` i natywny odczyt sesji po auto-apply oraz zapisie.

## P10 — Georeferencja, CP/GCP i trajektorie odniesienia

**Pliki:** `step2_georeference_operations.h/.cpp`, `core/src/{control_points,ground_control_points,gnss,tum}.cpp`, odpowiednie nagłówki oraz minimalne adaptery GUI; `processing/tests/georeference_operations_test.cpp`.
**Interfejs:** operacje import/edycja CP i GCP, `step2.align_reference`, istniejące GNSS/TUM/fuzja i obsługiwane układy/geoidy. CP i GCP mają osobne schematy.

- [ ] Test CP/GCP na znanej transformacji: lokalny punkt, cel, sigmy i wysokość sensora zachowują swoje znaczenie; test niepoprawnego powiązania z pozą i nieobsługiwanego układu.
- [ ] Wydzielić sztywne dopasowanie obecnie schowane pod `WITH_GUI`, bez jego kopiowania do Go. Udostępnić obecne importy/fuzje, zachowując jednostki i datum.
- [ ] Sprawdzić snapshot z niezapisanym CP/GCP i referencją trajektorii; wynik GUI/headless jest równoważny, brak geoidy jest jawnym błędem/deklarowanym ograniczeniem dostępności tej operacji.
- [ ] CTest `processing-georeference`; realne dane referencyjne dla operacji, których Parking bez GNSS nie pokrywa. Dobór takich danych z macierzy P00 jest warunkiem odbioru tej karty.

## P11 — Automatyczne kandydaty zamknięć pętli

**Pliki:** `step2_loop_candidate_operations.h/.cpp`, `bow_loop_closure_detector.h/.cpp`, odpowiednie wywołania GUI i zasoby CMake/pakowania; `processing/tests/loop_candidates_test.cpp`.
**Interfejs:** `constraints.detect_candidates` zwraca kandydatów; `constraints.add/replace` z P09 stosuje wybrany zestaw.

- [ ] Test wykrywania: aktywny graf pozostaje niezmieniony; kandydat zawiera jawne endpointy i dane potrzebne do jego oceny/dodania.
- [ ] Podłączyć istniejący BoW przez worker i zadanie GUI; użyć tej samej wersji OpenCV/fbow co reszta produktu.
- [ ] Sprawdzić słownik BoW w przeniesionej instalacji. Obecny CMake osadza ścieżkę source tree — jeśli nadal wymaga korekty, przygotować minimalny fix jako osobny commit i opisać zależność, zamiast rozbudowywać tę kartę o porządki pakowania. Otwarcie osobnego PR pozostaje do odrębnego zlecenia.
- [ ] CTest `processing-loop-candidates` i rzeczywiste wykrywanie na przygotowanym materiale, także bez dostępu do source tree.

## P12 — Jawne edycje segmentów i transformacje

**Pliki:** `step2_segment_operations.h/.cpp`, minimalne fragmenty `multi_view_tls_registration_gui.cpp`, istniejące funkcje trajektorii; `processing/tests/segment_operations_test.cpp`.
**Interfejs:** `step2.edit_segment`, `step2.transform_scope`, przywrócenie pozycji dla wskazanego zakresu; zakres i macierze są argumentami, nie stanem gizma.

- [ ] Test znanego segmentu: zmieniają się wyłącznie wskazane poses, jednostki i kolejność transformacji odpowiadają istniejącej operacji. Pusty/nieprawidłowy zakres daje błąd bez efektu ubocznego.
- [ ] Wydzielić dotychczasową logikę do wspólnego handlera, pozostawiając podgląd/przeciąganie w GUI. Stan w trakcie niezatwierdzonego przeciągania nie jest mylony z zatwierdzoną rewizją przetwarzania.
- [ ] Podłączyć API i istniejące zatwierdzanie edycji do wspólnej kontroli rewizji; wynik od razu widać w GUI.
- [ ] CTest `processing-segments` oraz ręczne potwierdzenie granic segmentu i zapisu/odczytu realnej sesji.

## P13 — Wszystkie istniejące importy i eksporty sesji

**Pliki:** `step2_io_operations.h/.cpp`, istniejące session/E57/LAZ/trajectory IO, `multi_view_tls_registration_gui.cpp`; `processing/tests/session_io_operations_test.cpp`.
**Interfejs:** `session.open/save/subsession`, `export_result` z formatem i jawnym zakresem; wyniki publikowane przez P05. Lista formatów jest zamkniętą listą z macierzy P00.

- [ ] Dodać roundtrip testy dla formatów z jasno określonym kontraktem oraz test dwóch skanów (jeden niewidoczny): jawne „oba” eksportuje oba, „widoczne” jest rozwiązywane tylko na żądanie.
- [ ] Podłączyć istniejące LAS/LAZ/E57, trajektorie i inne potwierdzone eksporty, w tym przekroje, do rejestru operacji. Zachować offsety, timestampy i identyfikatory skanów; nie wymyślać nowych formatów.
- [ ] Zostawić dotychczasowe `--export` i `--same-as-gui` w obecnym znaczeniu. Ich obsługa może korzystać ze wspólnych funkcji, ale nie zmienia się tryb wywołania ani nazwy opcji.
- [ ] CTest `processing-session-io`, natywny odczyt wyniku oraz porównanie dotychczasowego CLI na realnych danych.

## P14 — Stan projektu wielosesyjnego i kompletny zapis

**Pliki:** `apps/multi_session_registration/project_io.h/.cpp`, `project_operations.h/.cpp`, istniejące `multi_session_registration.cpp`; `processing/tests/project_io_test.cpp`.
**Interfejs:** `project.open/save/add_session/remove_session/set_ground_truth`, snapshot projektu z rewizjami zależnych sesji; integracja `PublishResult`.

- [ ] Test projektu z dwiema sesjami, ground truth i więzem: roundtrip zachowuje znaczenie; zwykły wynik ma własne `.mjp/.mjs/poses`, źródłowe poses pozostają bitowo niezmienione.
- [ ] Wydzielić loader/writer `.mjp` bez zmiany formatu i bez aktualizowania tytułu okna z warstwy IO. GUI osobno reaguje na wynik operacji.
- [ ] Podłączyć edycję listy sesji, ground truth i zapis do wspólnej warstwy; obecny binding step3 nie jest traktowany jako pełna rejestracja.
- [ ] CTest `processing-project-io`; test przerwania zapisu całego grafu zależności oraz otwarcie rzeczywistego projektu z przygotowanego podzbioru wielu sesji.

## P15 — Rejestracja i graf wielu sesji

**Pliki:** `step3_operations.h/.cpp`, istniejące `multi_session_factor_graph.h/.cpp`, `multi_session_registration.cpp`; `processing/tests/step3_operations_test.cpp`.
**Interfejs:** `project.transform_scope/register_sessions`, projektowe warianty `constraints.*`, `optimize_project`; numeryczne argumenty i jawne pary sesji.

- [ ] Test dwóch sesji: wskazana ground truth nie zmienia pozy; endpointy więzów są walidowane; wykrywanie kandydatów z progu bliskości nie czyści aktywnych `edges`; jawne replace zastępuje tylko wskazany zakres.
- [ ] Wydzielić istniejące NDT/ICP/generowanie więzów/optymalizację/przywrócenie pozycji do handlerów. CP/GCP zapisane w sesjach uczestniczą jak dziś; nie aktywować zakomentowanej sekcji GNSS jako części MCP.
- [ ] Podłączyć ciężkie przyciski GUI do koordynatora; wspólny zakres nie zależy od liczby widocznych sesji. Wynik zawiera pełny projekt z poses z P14/P05.
- [ ] CTest `processing-step3`; porównanie realnej rejestracji wielosesyjnej z bazowym wynikiem oraz zamknięcie GUI/ponowne połączenie podczas optymalizacji.

## P16 — Eksporty projektu i końcowy katalog operacji

**Pliki:** `step3_io_operations.h/.cpp`, minimalne istniejące eksporty w `multi_session_registration.cpp`, testy IO projektu; `internal/mcp/catalog.go`, testy katalogu i wyników.
**Interfejs:** projektowe warianty `export_result`, kompletne schematy narzędzi MCP oraz resources do raportów/artefaktów. Registry C++ pozostaje źródłem możliwości i parametrów.

- [ ] Podłączyć istniejące TUM/CSV/LAS/LAZ i zapisy poses do jawnego zakresu i publikacji. Pełny eksport projektu ma odtworzyć wszystkie zależności bez źródłowego katalogu.
- [ ] Dodać testy struktury wyników, paginacji list dużego projektu, błędów i kompletności katalogu wobec registry C++. Brak wysyłania pełnych chmur przez odpowiedzi JSON.
- [ ] Przejść macierz P00: każda istniejąca rodzina w zakresie ma implementację, GUI/headless i odpowiedni dowód; udokumentować realne ograniczenia istniejących algorytmów zamiast deklarować nieistniejącą funkcję.
- [ ] Go test katalogu, CTest `processing-project-export`, prawdziwe rozmowy narzędziowe Codex i Claude dla wszystkich etapów; nie uznawać samego `tools/list` za dowód wykonania workflow.

## P17 — Odbiór produktu i domyślne włączenie

**Pliki:** istniejące CMake/opcje, CI, `deploy_mandeye.bat`, `macos_bundle_toolkit/`, CPack, dokumentacja wiki; narzędzie automatyzujące powtarzalny odbiór tylko jeśli potrzebne, z wpisem w `scripts/README.md`.
**Interfejs:** gotowy pakiet HDMapping, konfiguracja lokalnego stdio w Codex i Claude, wspólna wersja wszystkich komponentów.

- [ ] Uzupełnić pokrycie pakowania wszystkich nowych executable i zasobów w istniejących ścieżkach dystrybucji; usunąć zależność runtime od build/source tree.
- [ ] Wykonać pełny przebieg realnych danych 1→2→3→eksport, oba sposoby uruchomienia; dwa równoległe zadania GUI/MCP i trzecie w kolejce, cancel, zamknięcie obu klientów i okien, restart i ponowne wykonanie po awarii.
- [ ] Sprawdzić zwykły wynik i samowystarczalny eksport natywnymi konsumentami z zainstalowanego pakietu. Sprawdzić readback raportów i artefaktów dla dokładnego SHA.
- [ ] Przejść macierz platform: build/test/package na trzech, test użytkowy Windows 11, Codex i Claude na platformach przez nie wspieranych. Na Ubuntu brak oficjalnego Claude nie może być opisany jako wykonany test tego klienta.
- [ ] Udokumentować nowy sposób wykonywania ciężkich przycisków GUI, zależności zwykłych wyników, limit, anulowanie i konfigurację hostów. Zmiana jest zauważalna dla użytkownika i musi być jasno opisana w PR.
- [ ] Po spełnieniu wszystkich kryteriów ustawić domyślne ON i ponownie sprawdzić czysty build ON/OFF. To gotowość do wydania, nie automatyczna zgoda na merge ani release.

## Polecenia i dowody w trakcie implementacji

```sh
cmake -B build-mcp -DCMAKE_BUILD_TYPE=Release -DBUILD_WITH_UTILITY_APPLICATION=ON -DBUILD_TESTING=ON -DBUILD_WITH_MCP=ON
cmake --build build-mcp --parallel
ctest --test-dir build-mcp --output-on-failure
cmake --install build-mcp --prefix /tmp/hdmapping-mcp-install
python3 scripts/run_check_clang_format.py
git diff --check
git diff --submodule
```

To szablon poleceń dla POSIX; Windows używa konfiguracji Release i katalogu tymczasowego odpowiedniego dla systemu. P01 włącza testy Go do CTest przy pomocy tego samego, wybranego przez CMake kompilatora. W samym module można dodatkowo uruchamiać `go test ./...` i `gofmt`, używając identycznej wersji. Filtry `ctest -L processing-…` powstają wraz z nazwanymi kartami. Test ma udowadniać zachowanie, nie utrwalać kroków rozumowania autora planu.

Przed każdym PR zmieniającym przetwarzanie: rzeczywiste nagranie i natywny odczyt, zgodnie z AGENTS.md. Przed scaleniem: terminalne CI dla dokładnego SHA i wymagany test Windows. Nie traktować `in_progress`, samego importu modułu ani powstałego ZIP jako dowodu sukcesu.

## Sprawdzenie pokrycia planu

| Wymagania | Karty |
| --- | --- |
| Q1/Q4/Q6 — lokalne MCP i hosty | P00, P01, P03, P16, P17 |
| Q2/Q3/Q5/Q20 — pełne 1–3, GUI i headless, wspólne przyciski | P06–P16 |
| Q7/Q11/Q16 — życie, zamknięcie, anulowanie, awaria | P02–P06, P17 |
| Q9/Q10/Q19 — wspólny stan, rewizje, zastosowanie wyniku | P04, P06, P07–P16 |
| Q12/Q13 — równoległość i limit | P02, P03, P17 |
| Q14/Q15 — zakres i więzy | P08, P09, P11–P16 |
| Q8/Q18 — osobny i pełny wynik | P04, P05, P13, P14, P16 |
| Q17 — Go/C++, jedno wydanie | P01, P03, P17 |

Źródło wymogu Go: [go.mod SDK v1.7.0](https://github.com/modelcontextprotocol/go-sdk/blob/v1.7.0/go.mod). Dostępność obsługi wersji protokołu nie zastępuje prób P00 w docelowych klientach.

Plan kończy się gotowością kompletnego rozwiązania do dostarczenia. Wymaga wykonania i dowodów wszystkich kart; nie obiecuje terminu ani nakładu bez P00 i pierwszych integracji. Rekomendowany przyszły sposób wykonania: kolejno, z niezależnym przeglądem każdej karty i całości, ze względu na wspólne kontrakty i trwałość danych.
