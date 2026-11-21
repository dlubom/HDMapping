# Rozpoznanie przed planem MCP

Sprawdzono 2026-11-15. Źródła lokalne: commit `3d3a8054986f95fcf580ef0f780889043bfdd475`. To analiza kodu, nie wynik uruchomienia ani buildu.

## Dostępne granice przetwarzania

| Obszar | Fakt i źródło |
| --- | --- |
| Krok 1 CLI | `apps/lidar_odometry_step_1/lidar_odometry_gui.cpp:2779`: wejście katalogowe, opcjonalne TOML i katalog wyjścia, `--dump-default-params`. Gałęzie obliczeniowe działają bez inicjalizacji GUI. |
| Krok 2 CLI | `apps/multi_view_tls_registration/multi_view_tls_registration_gui.cpp:7697`: `--export` obsługuje sesję `.mjs/.json`, globalne LAZ i CSV; nie jest pełnym API rejestracji. |
| Krok 2 Python | `apps/multi_view_tls_registration/multi_view_tls_registration.cpp:630`: optymalizacje NDT/ICP/pose graph. Nie wszystkie błędy mają jednoznaczny wynik API; część zapisów używa ścieżek przechowywanych w wejściowej sesji (`:720`). |
| Krok 3 Python | `apps/multi_session_registration/multi_session_registration_no_gui.cpp:745`: `run()` wczytuje, ustawia sesje do początku i zapisuje poses/TUM; nie wykonuje pełnej optymalizacji GUI. |
| Biblioteki | `core/CMakeLists.txt:84`: `core_no_gui` i współdzielone źródła; nie ma jednolitego API całego ciągu przetwarzania. |
| Raport | `apps/lidar_odometry_step_1/run_report.h:19`: raport kroku 1 ma stan, etapy, czasy i błędy. Nie jest ogólnym zarządcą zadań wszystkich aplikacji. |

## GUI

- Krok 1 ma globalny stan i jeden worker, a start jest obsługiwany na początku klatki (`lidar_odometry_gui.cpp:1873`, `:2075`). Podczas pracy blokuje część modyfikacji, pozostawiając podgląd. Porzucenie obecnego wykonania może zakończyć cały program (`:2034`); nie jest gotowym kontraktem anulowania zadania MCP.
- Krok 2 wywołuje część optymalizacji synchronicznie z przycisków (`multi_view_tls_registration_gui.cpp:1280`, `:1425`). Krok 3 ma podobny model (`multi_session_registration.cpp:606`).
- Krok 2 ma dodatkowy stan kontrolek NDT/ICP przepisywany do silnika w kolejnych klatkach (`multi_view_tls_registration_gui.cpp:981`, `:1251`). Zdalny zapis tylko do obiektu silnika nie zapewni zgodności z GUI.
- Nie znaleziono istniejącej warstwy MCP, rejestru instancji GUI, wspólnej kolejki poleceń ani rewizji całej sesji do wykrywania konfliktów.

Wniosek: adapter nad samymi obecnymi CLI/bindingami nie pokrywa uzgodnionego zakresu. Potrzebna będzie wspólna granica operacji i kontrola stanu dla GUI oraz pracy bez GUI. To wniosek z kodu, nie wybór architektury użytkownika.

## MCP — aktualne źródła

- [Specyfikacja 2026-07-28](https://modelcontextprotocol.io/specification/2026-07-28): obecny cel `/specification/latest`; JSON-RPC, narzędzia i zasoby, postęp/anulowanie oraz opcjonalne rozszerzenie Tasks. Wersję obsługiwaną w implementacji należy ustalić także względem Codex i Claude Desktop.
- [Transport stdio](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/stdio): klient uruchamia proces; `stdout` służy protokołowi. Istniejących tekstowych logów HDMapping nie można mieszać z komunikatami MCP.
- [Streamable HTTP](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http): umożliwia niezależne działanie serwera; transport nie przesądza sposobu zarządzania zadaniami HDMapping.
- [SDK](https://modelcontextprotocol.io/docs/2026-07-28/sdk): oficjalny katalog wymienia Python Tier 1, nie wymienia C++. Dobór C++ oznacza osobną ocenę biblioteki protokołu; Python oznacza runtime do dostarczenia wraz z produktem.
- [Python SDK v2](https://py.sdk.modelcontextprotocol.io/whats-new/): stabilna linia obsługuje nowy i starszy protokół. Nie implementuje jeszcze oficjalnego rozszerzenia Tasks; samo użycie SDK nie daje trwałego zadania. Obsługa ponownego połączenia wymaga osobnego kontraktu HDMapping.

## Granice dowodów

Nie uruchamiano pipeline, buildu, klienta MCP ani GUI. Nie odświeżano gałęzi zdalnej. Przed implementacją potrzebna będzie mapa operacji dostępnych w GUI 1–3 i ich dostępności bez GUI; przed odbiorem — rzeczywiste wykonania na danych oraz wszystkie trzy platformy.

## Mapa rodzin operacji

| Etap | Rodziny istniejące w produkcie |
| --- | --- |
| 1 | Nagranie LiDAR/IMU, presety/TOML, przygotowanie trajektorii/cache, odometria, consistency/smoothing, chmury referencyjne i wstępne dopasowanie, zapis sesji/raportu, LAS/LAZ i trajektorie ASCII. |
| 2 | Sesja/chmury/E57, NDT/ICP/plane features/pose graph, ręczne więzy i ICP więzów, kandydaci BoW, segmenty LIO i transformacje, CP/GCP, GNSS/NMEA/TUM, georeferencja, sesja/subsesja/E57, chmury i trajektorie. |
| 3 | Projekt `.mjp`, sesje i ground truth, transformacje początkowe, NDT między sesjami, więzy/ICP/optimizacja grafu, przywracanie pozycji, poses sesji, TUM/CSV/LAS/LAZ. |

Źródła: `lidar_odometry.h:19`, `lidar_odometry_gui.cpp:1204`, `core/include/Core/pose_graph_loop_closure.h:62`, `apps/multi_session_registration/multi_session_factor_graph.h:21`, `multi_session_registration.cpp:2329`, `:3124`.

Istotne rozróżnienia do dalszych decyzji:

- Eksport LAZ wybiera `pc.visible` (`core/include/Core/export_laz.h:502`), a dobór par sesji w GUI kroku 3 zależy od ich widoczności (`multi_session_registration.cpp:2306`). Zakres wejścia MCP musi być jednoznaczny mimo wyłączenia sterowania widokiem.
- Plik `.mjp` i zapis poses sesji to oddzielne operacje. Zapis poses używa ścieżek dotychczasowych sesji (`multi_session_registration.cpp:3148`). Nowy kompletny wynik wymaga zachowania referencji do nowych danych, bez ukrytego zapisu do wejść.
- Kandydaci BoW i aktywne więzy nie są tym samym. Obecne generowanie więzów kroku 3 czyści wcześniejsze `edges` (`multi_session_registration.cpp:1788`). W MCP trzeba wyraźnie opisać wybór dodania lub zastąpienia.
- CP i GCP to dwa różne modele (`control_points.h:8`, `ground_control_points.h:8`). Część dopasowania sztywnego znajduje się pod `WITH_GUI` (`control_points.cpp:142`, `ground_control_points.cpp:123`). Krok 3 wykorzystuje CP/GCP, lecz nie ma aktywnego odpowiednika bezpośredniej sekcji GNSS kroku 2.
- Obecne przeliczenia współrzędnych obejmują konkretny zestaw: PUWG92, UTM_AUTO, WebMercator i dostępne geoidy. Nie należy opisywać ich jako gotowej obsługi dowolnego EPSG.

## Klienci referencyjni

- Codex CLI 0.169.0: lokalne `codex mcp add --help` potwierdza komendę stdio i URL Streamable HTTP; [oficjalne docs](https://learn.chatgpt.com/docs/extend/mcp?surface=cli) opisują oba warianty.
- Claude Desktop: [lokalna konfiguracja](https://modelcontextprotocol.io/docs/2026-07-28/develop/connect-local-servers) uruchamia proces; [rozszerzenia](https://support.claude.com/en/articles/10949351-getting-started-with-local-mcp-servers-on-claude-desktop) mogą pakować lokalny serwer. [Remote connectors](https://support.claude.com/en/articles/11175166-get-started-with-custom-connectors-using-remote-mcp) łączą się z chmury, więc nie zastępują bezpośredniego połączenia do localhost.
- Udokumentowany wspólny transport lokalny: stdio; przyjęty następnie w Q17. Proces stdio może zniknąć wraz z klientem; zadania mają według Q7 żyć dłużej.
- Lista SDK obejmuje również Go Tier 1; [oficjalne Go SDK](https://github.com/modelcontextprotocol/go-sdk) deklaruje obsługę starszych wersji protokołu i 2026-07-28 od v1.7.0. To alternatywa do oceny razem z kosztem nowego toolchainu.
- Zgodność obu rzeczywistych hostów, platformy dostępności Claude Desktop oraz negocjowane wersje protokołu wymagają sprawdzenia podczas odbioru. Przegląd dokumentacji nie zastępuje testu klienta.

## Kandydaci do implementacji protokołu C++

Rozpoznanie źródeł upstream, bez lokalnego buildu ani testu interoperacyjności:

- [0xeb/fastmcpp](https://github.com/0xeb/fastmcpp): C++17/CMake, Apache-2.0, stdio, protokół do 2025-11-25, status beta. [CI z października 2026](https://github.com/0xeb/fastmcpp/actions/runs/39694884193) ma sukces Windows/Ubuntu Debug i Release oraz macOS Release, ale błędy macOS Debug i wariantu curl. Kandydat wymaga przypiętej rewizji i sprawdzenia trzech platform; nie został wybrany.
- [Neumann-Labs/mcp-cpp](https://github.com/Neumann-Labs/mcp-cpp): stdio wyłączone na Windows, więc nie spełnia wspólnego lokalnego transportu wybranego do porównania. README i LICENSE różnią się w oznaczeniu licencji; nie traktować jako zatwierdzonej zależności.

Warianty rozważone przed Q17 (użytkownik następnie wybrał Go + C++): natywna warstwa MCP C++ z kwalifikacją biblioteki społecznościowej albo cienki adapter na oficjalnym SDK Go i runtime przetwarzania C++. Go dodaje język/toolchain, C++ wymaga większej weryfikacji dojrzałości warstwy protokołu. Python pozostaje możliwy technicznie, ale wymaga dystrybucji runtime i zależności; obecne bindingi nie zapewniają pełnego zakresu.

## Dane do odbioru wielu sesji

[OmniWarsawDataset](https://github.com/MapsHD/OmniWarsawDataset), linkowany z README HDMapping, prowadzi do [publicznego archiwum Mandeye](https://archive.org/details/OmniWarsaw-lidar). Agent sprawdził centralny spis `warszawa.zip` przez HTTP Range (12,73 MB metadanych, bez chmur): 30 katalogów `continousScanning_0000`–`0029`, brak `.mjs`, `.mjp`, `.mrp`, `.mri`. Całość ma 181673765490 bajtów; nie pobrano jej.

To kandydat źródłowy, nie gotowy fixture kroku 3. Nie potwierdzono konkretnej pary nakładających się nagrań ani ich odczytu w bieżącym HDMapping. Przygotowanie rzeczywistego podzbioru, projektu i bazowego wyniku musi poprzedzać walidację kroku 3. Inne sprawdzone źródło [MandeyeMappingLoopClosure](https://huggingface.co/datasets/MichalPelka/MandeyeMappingLoopClosure) opisuje jedno nagranie; nie zastępuje zestawu wielu sesji.
