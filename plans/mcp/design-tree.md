# Planowanie MCP w HDMapping

Status: decyzje Q1–Q20 ustalone, plan zaakceptowany; właściciel zlecił commit/push na fork i pracę po jednej karcie w osobnym czacie. Bieżącą kartę autoryzuje jej prompt; zasady przekazania opisuje README.
Data rozpoczęcia: 2026-11-15.
Baza rozpoznania: `main`, commit `3d3a8054986f95fcf580ef0f780889043bfdd475`; drzewo śledzonych plików czyste.

## Cel potwierdzony przez użytkownika

- Q1: HDMapping udostępnia funkcje zewnętrznemu asystentowi przez Model Context Protocol (serwer MCP).
- Q2: od pierwszej wersji zarówno operacje bez GUI, jak i sterowanie otwartą aplikacją GUI.
- Q3: od początku cały ciąg kroków 1–3.
- Q4: jeden komputer i lokalne dane.
- Q5: pełne przetwarzanie w obu trybach: otwieranie danych, parametry, odometria, rejestracja, więzy między skanami/sesjami, zapis i eksport. Obsługa kamery, widoczności i zaznaczeń nie wchodzi do tego zakresu.
- Q6: odbiór w Codex i Claude Desktop.
- Q7: zadanie kontynuuje pracę po zamknięciu asystenta/zerwaniu połączenia; można wrócić do postępu i wyników.
- Q8: osobny wynik domyślnie; zastąpienie istniejącej sesji/projektu na jawne żądanie.
- Q9: wspólny bieżący stan GUI, także niezapisany; zmiany MCP widoczne w interfejsie.
- Q10: jedna mutacja danego obiektu naraz, podgląd dostępny; wykrywanie poleceń opartych na nieaktualnym stanie.
- Q11: zamknięcie okna HDMapping nie kończy zadania.
- Q12: kilka ciężkich zadań może działać równocześnie.
- Q13: konfigurowalny limit równoległości, domyślnie 2; nadmiarowe zadania oczekują w kolejce. Jedna mutacja danego stanu naraz; równoległość dotyczy odrębnych sesji/projektów albo ich kopii.
- Q14: jawny zakres danych; użycie widoczności GUI wyłącznie na wyraźne żądanie.
- Q15: wykrywanie kandydatów więzów, dodawanie i zastępowanie aktywnych więzów są oddzielnymi operacjami.
- Q16: anulowanie zadania bez zamykania GUI; po awarii komputera zadanie oznaczone jako przerwane, z możliwością ponownego wykonania od początku. Brak wymogu wznowienia od iteracji.
- Q17: Go dla adaptera MCP i koordynatora; C++ dla wspólnych operacji, procesów obliczeniowych i integracji GUI. Lokalny transport MCP stdio i niezależne procesy zadań zgodnie z przedstawioną architekturą. Wspólne repozytorium, build, wersja i wydanie; Go nie jest wymagane u użytkownika gotowego pakietu.
- Q18: zwykły wynik ma własne pliki projektu/sesji/poses, może współdzielić istniejące niezmienione chmury. Pełna kopia danych dostępna na żądanie.
- Q19: wynik zadania dla GUI automatycznie pojawia się w przypisanym oknie przy zgodnym stanie wejściowym; zapis do wcześniejszych plików osobno. Po zamknięciu okna wynik pozostaje do późniejszego otwarcia.
- Q20: ciężkie obliczenia uruchamiane istniejącymi przyciskami aktualnych GUI kroków 1–3 także korzystają z koordynatora, wspólnego limitu, kolejki i kontynuacji po zamknięciu okna.

## Bieżąca runda decyzji

Brak otwartych pytań zakresowych. Przegląd wykazał dwie luki: autor doprecyzował bezpieczne zamknięcie okna podczas `preparing` (proces pozostaje w tle do końca przekazania snapshotu), a użytkownik rozstrzygnął Q20 na rzecz wspólnego koordynatora dla GUI i MCP. Użytkownik zaakceptował plan i zlecił jego wersjonowanie oraz przygotowanie promptu P00. Kolejne karty uruchamia osobnymi promptami.

Rekomendacje asystenta przedstawione w pytaniach nie są decyzjami użytkownika.

## Zależności kolejnych decyzji

- Q2 + Q3 → konkretne operacje, ich wejścia i wyniki, stan sesji i relacja do GUI.
- Q4 → klienci MCP, transport i uruchamianie.
- Wybrane operacje → zapis wyników, zachowanie przy błędach, długie zadania i anulowanie.
- Operacje + uruchamianie + rozpoznanie kodu → warianty architektury, zależności i pakowanie.
- Powyższe decyzje → kryteria odbioru i etapy wdrożenia.
- Zakres przetwarzania + stan GUI → jawny wybór skanów/sesji, semantyka kandydatów i aktywnych więzów, kompletny zapis wyników projektu.
- Trwałość zadań → anulowanie, przerwany zapis, odzyskanie po awarii (odrębne od rozłączenia klienta).

## Obowiązujące ograniczenia repozytorium

- Windows 11, macOS i Ubuntu 24.04; wspólne wersjonowanie i wydawanie wszystkich narzędzi.
- Istniejące formaty, opcje CLI i wyniki obliczeń zachowują swoje znaczenie.
- Samowystarczalny build według dokumentacji, bez dodatkowych ręcznych etapów.
- Jedna odpowiedzialność na PR; testy logicznych kontraktów i sprawdzenie realnego nagrania przy zmianach przetwarzania.
- Właściciel forka zatwierdził wyjątek dla wersjonowanego `plans/mcp/`; pozostałe notatki zachowują zasady AGENTS.md. Dokumentacja użytkownika produktu należy do wiki.

## Zweryfikowane punkty wyjścia

- `CMakeLists.txt`: C++20, CMake 4.0, wersja produktu 0.106.0; Python bindings są opcjonalne (`BUILD_WITH_PYBIND`, domyślnie OFF).
- `pybind/CMakeLists.txt`: cztery moduły — `core_py`, `lidar_odometry_py`, `multi_view_tls_registration_py`, `multi_session_registration_py`; wiązanie do `core_no_gui`.
- `pybind/README.md`: przykłady uruchamiania odometrii i rejestracji wielowidokowej oraz zapisu wyników.
- `.github/workflows/python-bindings-{linux,windows}.yml`: budowanie modułów i sprawdzenie importu pierwszych trzech; nie jest to dowód wykonania całego przetwarzania.

## Dokumenty powstające w toku wywiadu

Projekt: `design.md`. Plan wdrożenia: `implementation-plan.md`, 18 kart P00–P17 z kryteriami odbioru. Słownik: `GLOSSARY.md`. Uzasadnienia dwóch decyzji architektonicznych: `adr/0001-independent-job-lifetime.md`, `adr/0002-go-coordinator-cpp-processing.md`. Rozpoznanie kodu i źródeł: `findings.md`. Dokumenty tego pakietu są wersjonowane na branchach MCP forka zgodnie z jawnym poleceniem właściciela.
