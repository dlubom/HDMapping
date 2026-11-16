# MCP — standard jakości Go i C++

Obowiązuje przy kwalifikacji, implementacji i odbiorze kart [P00–P17](implementation-plan.md). Uzupełnia `AGENTS.md`; zakres funkcjonalny i zasada jednej karty na czat pozostają bez zmian. To wymagania do wdrożenia w odpowiednich kartach, nie dowód, że narzędzia lub kontrole już działają.

## Wprowadzanie kontroli

| Karta | Wymagany wynik |
| --- | --- |
| P00 | Kwalifikacja zgodnych wersji Go, SDK i narzędzi jakości; zapis dokładnych wersji, źródeł, checksums pobieranych archiwów oraz wymagań platformowych. Próby w tymczasowym prototypie poza śledzonym drzewem; bez integracji CMake i CI produktu. |
| P01 | Konfiguracja formatowania, lintowania, testów Go, race detectora i govulncheck we wspólnym buildzie deweloperskim i istniejącym CI, przed rozbudową logiki koordynatora. Przygotowanie sposobu uruchamiania kontroli C++ dla pierwszych nowych targetów. |
| P03 | Pierwsze targety workera i `processing/` od razu objęte formatowaniem, ostrzeżeniami, clang-tidy, CTest oraz ASan/UBSan. TSan od pierwszego własnego kodu współbieżnego C++. |
| P02 i P04–P17 | Wszystkie kontrole wdrożone we wcześniejszych kartach, mające zastosowanie do bieżącego zakresu, oraz testy kontraktów zmienionych przez kartę. Raport wskazuje wykonany zakres i brakujące dowody. |

Wersje narzędzi i konfiguracja są wspólne dla pracy lokalnej i CI. Aktualizacja wersji jest świadomą zmianą z przeglądem nowych zgłoszeń. Narzędzia deweloperskie i sanitizery mają jawne opcje CMake; gotowy pakiet nie wymaga ich instalowania, a zwykły build zachowuje zasady samowystarczalności projektu. Nowe narzędzie w `scripts/` otrzymuje wpis w `scripts/README.md`.

Kontrole rozszerzają istniejące workflow HDMapping. Muszą rzeczywiście uruchamiać się dla dokładnego heada brancha MCP w forku, również gdy nie otwarto PR. Konfiguracja obejmuje właściwe zdarzenia/branche lub jawne uruchomienie istniejącego workflow. Brak uruchomienia nie jest sukcesem; MCP nie dostaje osobnego workflow ani procesu wydania.

Nowe moduły spełniają pełny przyjęty standard. Przy zmianie istniejącego kodu sprawdzaj dotknięte jednostki kompilacji i obszary oraz odróżniaj wcześniejsze zgłoszenia od nowych. Poboczne zaległości otrzymują osobny zakres pracy. Wyłączenie reguły jest wąskie, opisuje konkretną przyczynę i podlega przeglądowi; nie wyłącza całego nowego modułu ani kontroli zależnych od brakującego środowiska.

## Go

Od P01 polecenia Go uruchamiaj z katalogu modułu `apps/hdmapping_mcp/`. W P00 używaj modułu tymczasowego prototypu poza śledzonym drzewem.

| Kontrola | Wymaganie |
| --- | --- |
| Formatowanie | `gofmt`; CI kończy się błędem przy różnicach. Samo wypisanie plików przez `gofmt -l` nie wystarcza. |
| Analiza statyczna | Przypięty `golangci-lint v2`, `version: "2"`, `linters.default: none`, jawnie `govet`, `staticcheck`, `errcheck`, `unused`; `gofmt` w sekcji `formatters`. Jeden runner, bez dodatkowego osobnego Staticcheck i pełnego vet. |
| Testy | `go test ./...` na Windows 11, macOS i Ubuntu 24.04; także testy właściwe dla danej platformy. |
| Wyścigi | `go test -race ./...` obowiązkowo na Linux amd64. Zapewnij cgo i kompilator C; dodatkowe platformy wymagają kwalifikacji ich zależności. |
| Podatności | Przypięty `govulncheck ./...`; jawna macierz konfiguracji platformowych i build tags używanych przez produkt. Raportuj podatne osiągalne wywołania i rozstrzygnięcie zgłoszeń. Ta kontrola nie obejmuje silnika C++. |
| Fuzzing | Od pierwszego własnego parsera protokołu lub trwałego stanu: zapisany korpus regresyjny w zwykłych testach i aktywny fuzzing z jawnym limitem `-fuzztime`. Zachowuj minimalny reproduktor znalezionego błędu. |

Wytyczne kodu:

- Pakiety mają konkretną odpowiedzialność. Małe interfejsy wynikają z potrzeb konsumenta; unikaj interfejsu dla każdej struktury i warstw bez osobnej odpowiedzialności.
- Błędy wejścia, I/O i procesu zwracaj z kontekstem operacji; zachowuj przyczynę przez `%w` oraz rozpoznawaj ją przez `errors.Is/As`. `panic` nie służy do obsługi spodziewanych błędów użytkownika.
- `context.Context` przekazuj jawnie, jako pierwszy argument tam, gdzie operacja go potrzebuje. Wywołuj otrzymane funkcje `cancel`. Kontekst żądania MCP jest oddzielony od jawnie zarządzanego czasu życia trwałego zadania; rozłączenie klienta nie anuluje obliczeń.
- Każda goroutine ma właściciela, warunek zakończenia i sposób oczekiwania na koniec. Kolejki i współbieżność są ograniczone; własność mutowalnego stanu jest jednoznaczna.
- Pliki, strumienie i procesy mają określone sprzątanie. Sprawdzaj błędy zapisu i zamknięcia wpływające na poprawność wyniku; nie gromadź otwartych zasobów przez `defer` w długich pętlach.
- Stdout adaptera stdio zawiera wyłącznie protokół. Logi kieruj na stderr lub do ustalonego pliku; przechwytuj również zwykłe wyjście workera C++.
- Go odpowiada za transport i koordynację. Parametry, walidacja domenowa i algorytmy korzystają z kontraktów C++; nie twórz ich drugiej implementacji w Go.

## C++

| Kontrola | Wymaganie |
| --- | --- |
| Formatowanie | clang-format 21 z istniejącą `.clang-format`. Rozszerz wspólny zakres skryptów o nowe katalogi, w szczególności `processing/`, obecnie pomijany. |
| Analiza statyczna | Przypięty clang-tidy; rzeczywiste flagi C++20 z `compile_commands.json` generowanego przez CMake. Profil obejmuje `clang-analyzer-*` oraz jawnie wybrane reguły `bugprone` i `performance`; bez włączania wszystkich reguł stylu. |
| Ostrzeżenia | Dla nowych własnych targetów: GCC/Clang `-Wall -Wextra -Wpedantic`, MSVC `/W4`; kontrolowane CI wymaga także `-Werror` lub `/WX`. Ustawienia mają zakres targetu i nie narzucają tych wymagań third-party. |
| Testy | doctest i CTest z `BUILD_TESTING=ON` na Windows 11, macOS i Ubuntu 24.04. Zachowaj istniejące testy przepływu danych. |
| Pamięć i UB | Osobna konfiguracja Linux/Clang z ASan + UBSan dla nowych modułów i ich testów; prawidłowa instrumentacja kompilacji i linkowania. |
| Wyścigi | Osobna konfiguracja Linux/Clang z TSan dla własnego kodu współbieżnego; nie łącz jej z ASan. Raportuj zakres instrumentacji, w tym ograniczenia nieinstrumentowanych zależności. |

Wytyczne kodu:

- Stosuj RAII dla pamięci, plików, blokad i procesów. Preferuj wartości i `std::unique_ptr`; `std::shared_ptr` wymaga rzeczywistej współdzielonej własności. Surowy wskaźnik lub referencja oznacza pożyczony dostęp.
- Czas życia widoków i danych jest jawny. Ograniczaj zbędne kopie chmur, zachowując własność potrzebną do bezpiecznej pracy w tle; nie przechowuj widoku po zniszczeniu jego właściciela.
- Dane zewnętrzne waliduj i zwracaj opisany błąd zgodnie z istniejącym kontraktem `std::variant<..., OperationError>`. `HDMAPPING_ASSERT` sprawdza wewnętrzne niezmienniki. Wyjątki na granicy procesu/operacji zamieniaj na kontrolowany wynik błędu.
- Zmiany stanu GUI wykonuje jego wątek. Obliczenia korzystają z uzgodnionych snapshotów, rewizji, synchronizacji i anulowania; wątki mają jawny czas życia i są dołączane przed zwolnieniem ich danych.
- Zachowuj jednostki, układy współrzędnych, precyzję i tolerancje numeryczne. Sprawdzaj zakresy i wartości niefinitywne na wejściu oraz uzasadniaj zawężające konwersje. Refaktoryzacja nie zmienia wyników obliczeń.
- Przed dodaniem typu lub helpera przeszukaj `core/` i `shared/`; korzystaj z istniejących mechanizmów Eigen/raylib. Ścieżki buduj przez `std::filesystem::path` i `operator/`.
- Klasa z własną odpowiedzialnością ma własne `.h`/`.cpp` i krótki komentarz `//!` na deklaracji. Interfejs opisuje kontrakt, własność i istotne jednostki.

## Testy kontraktów i odbiór

Testuj obserwowalne zachowanie modułów i procesów. W miarę dostarczania właściwych kart wymagane scenariusze obejmują:

- idempotencję żądania, limit równoległości, rezerwację stanu i odrzucanie nieaktualnej rewizji;
- rozłączenie MCP, zamknięcie GUI, ponowne połączenie, anulowanie i awarię workera/koordynatora;
- przerwany zapis i publikację: poprzedni wynik pozostaje spójny, częściowy nowy wynik nie jest sukcesem;
- protokół Go–C++, nieznane lub uszkodzone komunikaty, limity wejścia i brak logów na kanale protokołu;
- ścieżki ze spacjami i znakami Unicode, błędy I/O oraz uruchomienie z przeniesionej instalacji;
- zgodność obliczeń na rzeczywistych danych i natywny odczyt wyników przy zmianach przetwarzania.

Synchronizacja testów współbieżności opiera się na sterowanych zdarzeniach i ograniczonym czasie oczekiwania, nie przypadkowych opóźnieniach. Testy nie odwzorowują struktury implementacji. Rendering i UX sprawdzaj w rzeczywistej aplikacji zgodnie z `AGENTS.md`; testuj kontrakt mostu GUI bez zrzutów ekranu. Raport pokrycia pomaga znaleźć nieprzetestowane gałęzie błędów; na start nie ma arbitralnego globalnego progu procentowego.

Przy kwalifikacji narzędzi w P00 oraz podłączeniu ich do CI w P01/P03 wykonaj tymczasowe próby kontrolne: naruszenie formatu, wykrywalny błąd analizatora i błąd testu muszą spowodować niepowodzenie właściwego polecenia. Dla sanitizerów/race potwierdź działanie na małym odpowiednim reproduktorze. Próby pozostają poza kodem produktu; usuwasz je przed docelową weryfikacją. Sprawdź, że kontrole obejmują rzeczywiste pliki, testy są odkrywane, a kod wyjścia nie jest maskowany.

Raport karty podaje dla każdej obowiązującej kontroli `PASS`, `FAIL` albo `NOT RUN`, platformę/architekturę, wersje narzędzi, konfigurację, polecenie, zakres oraz dowód dla dokładnego commita. Wymagane sprawdzenie niewykonane lub trwające pozostawia kartę `INCOMPLETE`. Warunkowa kontrola, której kod jeszcze nie istnieje, jest jawnie oznaczona jako nieobowiązująca z uzasadnieniem, bez fikcyjnego `PASS`. Samo dodanie konfiguracji nie zalicza kontroli. Zmiany wyłącznie dokumentacyjne wymagają przeglądu spójności, odsyłaczy i diffu oraz `[skip ci]`, bez budowania produktu.

## Źródła zasad i narzędzi

- [Konfiguracja golangci-lint v2](https://golangci-lint.run/docs/configuration/file/), [Go Code Review Comments](https://go.dev/wiki/CodeReviewComments), [`context`](https://pkg.go.dev/context).
- [Go race detector](https://go.dev/doc/articles/race_detector), [Go Vulnerability Management](https://go.dev/doc/security/vuln/), [Go fuzzing](https://go.dev/doc/security/fuzz/).
- [clang-tidy](https://clang.llvm.org/extra/clang-tidy/), [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html), [UndefinedBehaviorSanitizer](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html), [ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html).
- [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines).
