# Status MCP

Plan zaakceptowany. P00 wykonano częściowo; wynik **INCOMPLETE**. Właściciel 2026-11-16 polecił odłożyć kwalifikację Windows, Linux i klienta Claude oraz przejść dalej: **P01 rozpoczęto mimo niepełnego P00; implementacja, dostępne kontrole platformowe i publikacja wiki są dostarczane jako INCOMPLETE**. Szczegóły, dowody i zakres tej decyzji: [raport P00](results/P00.md#decyzja-właściciela-po-raporcie).

Uzgodniony [standard jakości Go/C++](quality.md) jest częścią planu: P00 kwalifikuje narzędzia, P01 wdraża kontrole Go, P03 obejmuje nimi nowe moduły C++. Wdrożone kontrole Go i ich wykonany zakres opisuje raport P01; samo dodanie konfiguracji nie zalicza pełnej macierzy.

**Zakres odbioru uzgodniony z właścicielem.** Od P01 odbiór automatyczny korzysta z [macierzy GitHub Actions](quality.md#macierz-github-actions), w tym Windows Server x64. Ręczne próby użytkowe na różnych maszynach są osobnym odbiorem.

**Decyzja właściciela 2026-11-16:** testy rzeczywistego klienta Claude odkładamy do końcowego odbioru P17. Właściciel nie ma obecnie Claude Code; nie instalujemy ani nie kwalifikujemy teraz klienta Claude. W uzgodnionym planie hostem jest Claude Desktop — odroczenie nie zamienia go automatycznie na Claude Code. Brak klienta nie blokuje prac nad osobno zleconymi kartami; jego testy pozostają `NOT RUN` i muszą wrócić w P17 przed pełnym odbiorem oraz domyślnym włączeniem MCP. Pozostałe braki P00/P01 i ograniczenie jednej karty na czat pozostają jawne.

**Decyzja właściciela 2026-11-16 — P02:** zlecono rozpoczęcie wyłącznie P02 mimo niepełnego odbioru P01, od `2a92ad363a0b0603e81d2381076975223c835788`, na wspólnej gałęzi `mcp/development` w istniejącym draft PR #1. Brakujące macOS ARM64 ON/OFF P01 odkładamy; starego runu P01 nie ponawiamy. P01 pozostaje **INCOMPLETE**. To jawne odstępstwo od kolejności pełnego odbioru, bez zaliczenia brakujących kontroli i bez zgody na P03. P02 podlega własnym kontrolom i bieżącemu CI; braki P00/danych pozostają odłożone, Claude na P17.

- Repozytorium do zapisu: `dlubom/HDMapping`.
- Branch planu: `mcp/implementation-plan`.
- Bazowy commit rozpoznania: `3d3a8054986f95fcf580ef0f780889043bfdd475`.
- Baza P01: `8d8ec2a15099e13dba5237b16a5e70a2b324b2fd`; kod historycznej macierzy P01: `46974875db17aabe17b1a292eb314df1abdfd65f`; poprawka sprawdzianu relokacji: `53204b7e68050d8aa577e171142290a3031f533b`; wspólna gałąź utworzona od `bb8b988b9be6dd26fc2541580bce2edfb962144e`.
- Branch kwalifikacji: `mcp/p00`.
- Decyzja właściciela 2026-11-16: wspólna gałąź `mcp/development` i jeden [draft PR #1](https://github.com/dlubom/HDMapping/pull/1) do `main` wyłącznie w forku. Gałąź utworzona z `bb8b988b9be6dd26fc2541580bce2edfb962144e`; `mcp/p01` zachowany. Ta zmiana sposobu dostarczenia nie upoważnia do merge, release ani rozpoczęcia kolejnych kart.
- **P02 PASSED** dla kodu `0cdbb9ae257f072c7040d948112838f6e2fe931a` na `mcp/development`. Niezależny przegląd wykrył kolizje kluczy rejestru po zapisie niepoprawnego UTF-8. Poprawka odrzuca takie klucze przed przyjęciem, a regresja potwierdza ponowny odczyt i zachowanie ID. Pełna macierz **8/8 PASS** obejmuje Windows Server x64, Ubuntu bundled, Debian i macOS ARM64 ON/OFF. Kontrole Go, fuzzing, govulncheck, Linux amd64 race, clang-format i próba adaptera z opublikowanego DMG również mają PASS. Poprzedni run macOS P02 `43945389834` został zastąpiony przez CI poprawki. Dowody opisano w [raporcie P02](results/P02.md#kontynuacja-odbioru). Następny prompt dotyczy wyłącznie **P03** na tej samej gałęzi w [draft PR #1](https://github.com/dlubom/HDMapping/pull/1). W przekazaniu P02 karta P03 była jeszcze NOT STARTED.
- **Decyzja właściciela 2026-11-17 — P03:** wykonanie wyłącznie P03 na `mcp/development`, baza `369e00f9f2940931588f700b9b7ade04185bb71f`, w [draft PR #1](https://github.com/dlubom/HDMapping/pull/1). P00 i P01 pozostają INCOMPLETE. Odłożone próby hostów Windows i Ubuntu, Linux TSan P00, braki danych Parking i kroku 3 oraz macOS ARM64 ON/OFF historycznego P01 pozostają w dotychczasowym zakresie. Run P01 `43886034883` pozostaje bez ponowienia. Claude Desktop przypada na P17. P03 przechodzi własny odbiór procesowy, kontrole jakości i macierz natywną, w tym Windows Server x64.
- **P03 PASSED** dla `5719364c078368bbe6095e4f4975fe655cadb4c1`. Koordynator, IPC, nadzór i worker przeszły integrację procesową i macierz **8/8 PASS** na Windows Server x64, Ubuntu bundled, Debian i macOS ARM64. **PASS** również Go race/fuzz/govulncheck, clang-format, clang-tidy, CTest `processing-runtime`, ASan/UBSan, osobny TSan i relokacje, w tym uruchomienie z pakietu z zamontowanego DMG. [Raport P03](results/P03.md) zawiera dokładne joby, ograniczenia i dowody. Przekazanie P03 otworzyło P04 w tym samym drafcie.
- **P04 PASSED** dla `2fb1599679d4b655659aa1ecb9825a1bc760f497` na `mcp/development`. Macierz **8/8 PASS** obejmuje Windows Server x64, Ubuntu bundled, Debian i macOS ARM64 ON/OFF z relokacjami, w tym bundle i DMG na macOS. Go race/fuzz/govulncheck, clang-format, clang-tidy, ASan/UBSan i osobny Linux TSan przeszły. Przegląd wykrył utratę punktów po zmianie ustawienia cache. Poprawka zachowuje źródło odroczonego odczytu LAS/LAZ, a test Parking ponownie potwierdził odczyt niezapisanych póz po zamknięciu GUI. Ograniczenia i dowody opisano w [raporcie P04](results/P04.md). Następny prompt dotyczy wyłącznie P05 w istniejącym draft PR #1.
- **P05 INCOMPLETE** dla kodu `4052646e88781d91fc10be50e56ecd9f7be9ab3b` w [draft PR #1](https://github.com/dlubom/HDMapping/pull/1). Publikacja i pełny eksport przeszły CTest, rzeczywisty odczyt Parking po zakończeniu producenta i odłączeniu wejścia, kontrolę ENOSPC oraz niezależny przegląd. Hosted clang-format i Debian OFF przeszły. Pozostałe joby wymagają zakończonego odbioru, w tym macOS ARM64 ON/OFF z brakami przydziału runnera oraz Linux Go race, ASan/UBSan i osobny TSan. [Raport P05](results/P05.md) podaje scenariusze, logi i dokładne braki. Kolejny prompt kontynuuje P05.
- Historyczne przekazanie P01 wskazywało **kontynuację P01 na `mcp/development`**, w istniejącym [draft PR #1](https://github.com/dlubom/HDMapping/pull/1), od zdalnego SHA z przekazania. Do odbioru P01 brakuje macOS ARM64 ON/OFF w Actions. Windows Server x64 zalicza weryfikację Windows w tej macierzy. Claude pozostaje na P17. P02 miało wtedy status `NOT STARTED`. Decyzja z 16 listopada dopuściła rozpoczęcie P02.
- Dowody P01: dla kodu `53204b7e68050d8aa577e171142290a3031f533b` **6/8 wariantów CI PASS**: Ubuntu bundled, Debian i Windows Server ON/OFF. PASS także kontrole Go, clang-format i Linux amd64 race. Bindingi Linux/Windows mają osobny PASS dla syntetycznego merge `8215547c8b30176e89d2893901bae953faa6f2d3`. Hosted macOS: [próba 5](https://github.com/dlubom/HDMapping/actions/runs/43886034883/attempts/5) ponowiła tylko brakujące ON/OFF; start **19:19:25 UTC**, koniec **19:34:29 UTC**. Joby ON `133548061523` i OFF `133548060859` zakończyły się `completed/cancelled`, `runner_id: 0`, `steps: []` (**NOT RUN**; workflow **FAIL infrastruktury**). Adnotacje potwierdzają brak przydziału i ograniczoną pojemność macOS ARM64. Checkout nie wykonał się; oba logi zwracają 404 `BlobNotFound`, brak artefaktów. Nie uruchamiano próby 6. Nie ponawiano zaliczonych kontroli. Kontynuacja od `9fcde67f8ad93ee03fa26d4d18730b89a2c396b3` zmienia tylko raport/status; przegląd wymagań nie wykazał nowej usterki. Potwierdzono ponownie checkout i końcowe logi zaliczonych jobów oraz 11/11 sum logów i pakietów. [Raport P01](results/P01.md#kontynuacja-odbioru--próba-5-macos) rozdziela bieżące CI, lokalne sprawdzenia kodu `53204b7` i starszy hosted macOS ON/DMG dla `4697487`.
- Braki P00 (bez ponawiania w tej kontynuacji): Windows 11, Claude Desktop, pełny test klienta/GUI Ubuntu, działający Linux TSan, potwierdzone nakładanie i bazowy projekt kroku 3 oraz zapis/odczyt wyniku optymalizacji GUI kroku 2.
- Odroczenie nie zmienia wyników kontroli ani wymagań przenośności i nie upoważnia do usuwania istniejących kontroli CI. Braki danych pozostają otwarte; nie są dowodem zgodności przyszłych zmian przetwarzania.
- Kolejne czaty stosują [zasady przekazania](README.md).

| Karta | Stan | Raport |
| --- | --- | --- |
| P00 | INCOMPLETE | [P00](results/P00.md) |
| P01 | INCOMPLETE | [P01](results/P01.md) |
| P02 | PASSED | [P02](results/P02.md) |
| P03 | PASSED | [P03](results/P03.md) |
| P04 | PASSED | [P04](results/P04.md) |
| P05 | INCOMPLETE | [P05](results/P05.md) |
| P06 | NOT STARTED | — |
| P07 | NOT STARTED | — |
| P08 | NOT STARTED | — |
| P09 | NOT STARTED | — |
| P10 | NOT STARTED | — |
| P11 | NOT STARTED | — |
| P12 | NOT STARTED | — |
| P13 | NOT STARTED | — |
| P14 | NOT STARTED | — |
| P15 | NOT STARTED | — |
| P16 | NOT STARTED | — |
| P17 | NOT STARTED | — |

Dozwolone stany: `NOT STARTED`, `IN PROGRESS`, `INCOMPLETE`, `PASSED`. Raport musi uzasadniać zmianę stanu. Sam commit, push lub ukończenie części sprawdzeń nie oznacza zaliczenia karty.
