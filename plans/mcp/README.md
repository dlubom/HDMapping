# MCP — praca po jednej karcie

Uzgodniony plan dla forka [dlubom/HDMapping](https://github.com/dlubom/HDMapping). Właściciel zlecił wersjonowanie planu i realizację każdej karty w osobnym czacie. Wklejenie promptu uruchamia wyłącznie wskazaną kartę; po jej zakończeniu agent przygotowuje prompt kolejnego czatu i zatrzymuje się.

## Dokumenty

- [Plan P00–P17](implementation-plan.md): zakres, zależności, interfejsy i kryteria odbioru.
- [Standard jakości Go i C++](quality.md): obowiązkowe kontrole, wytyczne i moment ich wdrożenia.
- [Projekt](design.md), [decyzje Q1–Q20](design-tree.md), [słownik](GLOSSARY.md) i [ADR](adr/0002-go-coordinator-cpp-processing.md): uzgodnione zachowanie.
- [Rozpoznanie](findings.md): dowody ze stanu bazowego; wersje bibliotek i hostów trzeba sprawdzić ponownie w P00.
- [Status kart](status.md): jedna trwała informacja o postępie, uaktualniana po pracy nad kartą.
- [Prompt P00](prompts/P00.md) oraz [szablon przekazania](prompts/next-task-template.md).

## Początek każdego czatu

1. Przeczytaj `AGENTS.md`, ten plik, `quality.md`, status, projekt oraz ograniczenia i sekcję wskazanej karty w planie. Czytaj pozostałe karty tylko dla potrzeb jej zależności.
2. Sprawdź rzeczywisty status Git, remotes i zdalny SHA brancha podanego w prompcie. Fork do zapisu to `dlubom/HDMapping`; identyfikuj go po URL, nie samym słowie `origin`. Zachowaj cudze zmiany.
3. Porównaj zdalny SHA z przekazanym. Jeśli branch przesunął się, sprawdź nowe commity i raporty przed kontynuacją. Nie resetuj ani nie nadpisuj historii, żeby odtworzyć starszy stan.
4. Nowa karta powstaje na osobnym branchu `mcp/pXX` od zweryfikowanego końca poprzedniej karty. P00 zaczyna od `mcp/implementation-plan`. Kontynuacja nieukończonej karty używa jej aktualnego brancha. Przy istniejącym branchu sprawdź zawartość zamiast go zastępować. Jeśli przekazano nowszą bazę planu, włącz jej uzgodnione zmiany przez fast-forward lub merge, zachowując pracę karty.
5. Wykonaj tylko wskazaną kartę. Podagenci mogą wykonywać rozpoznanie, implementację i przegląd w jej obrębie; nie rozpoczynają następnych kart. Przy zmianach kodu stosuj izolowany worktree, jeżeli jest potrzebny do zachowania istniejącej pracy.

## Odbiór i dostarczenie jednej karty

Każde kryterium ma wynik `PASS`, `FAIL` albo `NOT RUN`, z poleceniem/scenariuszem, środowiskiem i dowodem. Karta jest `PASSED` dopiero wtedy, gdy wszystkie jej wymagane kryteria mają `PASS`. Brak dostępu do Windows, klienta lub danych oznacza `INCOMPLETE`, nie zastępczy sukces. Test klientem SDK nie udaje testu rzeczywistego hosta.

Wykonaj wszystkie dostępne sprawdzenia przed raportowaniem ograniczenia. P00 nie przechodzi do P01 bez dowodu działania cyklu życia procesów na Windows 11, macOS i Ubuntu 24.04 oraz pozostałych wymaganych prób. Klienty AI sprawdzaj na oficjalnie wspieranych platformach.

Do kryteriów każdej karty należą również właściwe dla jej etapu kontrole z [quality.md](quality.md). P00 kwalifikuje narzędzia; P01 wdraża kontrole Go, a P03 obejmuje analizą i sanitizerami pierwsze nowe moduły C++. Dalsze karty utrzymują te kontrole. Samo zapisanie standardu w dokumentacji nie oznacza wdrożenia ani zaliczenia kontroli.

Zapisz zwięzły, odtwarzalny raport `plans/mcp/results/PXX.md` i uaktualnij status. Raport zawiera:

- kartę, branch, bazowy SHA i zakres zmienionych plików;
- macierz kryteriów: wynik, platforma/wersje, komenda lub scenariusz, dowód;
- kontrole jakości właściwe dla karty: wynik, objęte moduły, wersje narzędzi, badany SHA i dowód; jawne braki oraz uzasadnienia dla kontroli, których zakres nie dotyczy;
- wybrane wersje, checksums, konfigurację i pochodzenie danych;
- pozostałe braki oraz jednoznacznie `PASSED` albo `INCOMPLETE`;
- bazę do następnego czatu i informację, czy dotyczy on następnej karty, czy kontynuacji bieżącej.

Duże logi, pomiary, prototypy i dane pozostają poza śledzonym drzewem. Raport musi przenosić istotne ustalenia: komendy, źródła, identyfikatory i checksums. Sama lokalna ścieżka nie wystarcza; dla dowodu nieodtwarzalnego zapewnij dostępny artefakt i jego hash. Nie publikuj sekretów ani prywatnych danych pomiarowych. Kod produktu i komunikaty commitów nie zależą od prywatnych notatek.

Po przeglądzie zmian sprawdź `git diff --check` i `git diff --submodule`. Zatwierdź wyłącznie zmiany bieżącej karty i wykonaj zwykły push na jej branch w forku. Commity wyłącznie dokumentacyjne mają `[skip ci]`. Odczytaj zdalny SHA i zweryfikuj opublikowany raport; sprawdzenia dla kodu muszą dotyczyć dokładnego SHA. Jeżeli wymagane CI nadal trwa, nie opisuj go jako zielonego.

Prompt zlecający kartę według tych zasad obejmuje commit/push na fork oraz włączenie przekazanej bazy do brancha karty. Otwarcie lub scalenie PR, scalenie do `main`, push do upstream i release wymagają osobnego polecenia. Przy zmianach przetwarzania przed PR nadal obowiązują realne dane i natywny odczyt z AGENTS.md.

## Obowiązkowy prompt następnego czatu

W końcowej odpowiedzi podaj wynik karty, link do brancha/raportu, dokładny zdalny SHA, wykonane sprawdzenia i braki, a następnie **gotowy do skopiowania prompt** według szablonu. Wstaw faktyczne wartości, nie pozostawiaj pól do uzupełnienia.

- `PASSED`: prompt dotyczy tylko następnej karty liczbowo, na bazie właśnie zweryfikowanego remote SHA.
- `INCOMPLETE`: prompt dotyczy dalszej pracy nad tą samą kartą, wymienia konkretne brakujące warunki i dostępne dowody. Nie omija bramy odbioru.
- Po P17: prompt przekazania gotowego zakresu do końcowego odbioru, bez dopisywania P18 ani zlecania merge/release.

W każdym kolejnym prompcie wskaż `plans/mcp/quality.md` i powtórz zasadę: **jedna wskazana karta, wymagane kontrole jakości, raport, commit/push, następny prompt i stop**. Kolejnej karty nigdy nie wykonuj w bieżącym czacie. Końcowy SHA podaj po commicie i pushu; nie próbuj zapisywać w commicie jego własnego SHA.
