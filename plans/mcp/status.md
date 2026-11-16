# Status MCP

Plan zaakceptowany. P00 wykonano częściowo; wynik **INCOMPLETE**. Właściciel 2026-11-16 polecił odłożyć kwalifikację Windows, Linux i klienta Claude oraz przejść dalej: **P01 rozpoczęto mimo niepełnego P00; implementacja, dostępne kontrole platformowe i publikacja wiki są dostarczane jako INCOMPLETE**. Szczegóły, dowody i zakres tej decyzji: [raport P00](results/P00.md#decyzja-właściciela-po-raporcie).

Uzgodniony [standard jakości Go/C++](quality.md) jest częścią planu: P00 kwalifikuje narzędzia, P01 wdraża kontrole Go, P03 obejmuje nimi nowe moduły C++. Wdrożone kontrole Go i ich wykonany zakres opisuje raport P01; samo dodanie konfiguracji nie zalicza pełnej macierzy.

**Zakres odbioru uzgodniony z właścicielem.** Od P01 odbiór automatyczny korzysta z [macierzy GitHub Actions](quality.md#macierz-github-actions), w tym Windows Server x64. Ręczne próby użytkowe na różnych maszynach są osobnym odbiorem.

**Decyzja właściciela 2026-11-16:** testy rzeczywistego klienta Claude odkładamy do końcowego odbioru P17. Właściciel nie ma obecnie Claude Code; nie instalujemy ani nie kwalifikujemy teraz klienta Claude. W uzgodnionym planie hostem jest Claude Desktop — odroczenie nie zamienia go automatycznie na Claude Code. Brak klienta nie blokuje prac nad osobno zleconymi kartami; jego testy pozostają `NOT RUN` i muszą wrócić w P17 przed pełnym odbiorem oraz domyślnym włączeniem MCP. Pozostałe braki P00/P01 i ograniczenie jednej karty na czat pozostają jawne.

- Repozytorium do zapisu: `dlubom/HDMapping`.
- Branch planu: `mcp/implementation-plan`.
- Bazowy commit rozpoznania: `3d3a8054986f95fcf580ef0f780889043bfdd475`.
- Baza P01: `8d8ec2a15099e13dba5237b16a5e70a2b324b2fd`; kod historycznej macierzy P01: `46974875db17aabe17b1a292eb314df1abdfd65f`; poprawka sprawdzianu relokacji: `53204b7e68050d8aa577e171142290a3031f533b`; wspólna gałąź utworzona od `bb8b988b9be6dd26fc2541580bce2edfb962144e`.
- Branch kwalifikacji: `mcp/p00`.
- Decyzja właściciela 2026-11-16: wspólna gałąź `mcp/development` i jeden [draft PR #1](https://github.com/dlubom/HDMapping/pull/1) do `main` wyłącznie w forku. Gałąź utworzona z `bb8b988b9be6dd26fc2541580bce2edfb962144e`; `mcp/p01` zachowany. Ta zmiana sposobu dostarczenia nie upoważnia do merge, release ani rozpoczęcia kolejnych kart.
- Następna praca to **kontynuacja P01 na `mcp/development`**, w istniejącym [draft PR #1](https://github.com/dlubom/HDMapping/pull/1), od zdalnego SHA z przekazania. Do odbioru P01 brakuje macOS ARM64 ON/OFF w Actions. Windows Server x64 zalicza weryfikację Windows w tej macierzy. Claude pozostaje na P17. P02 ma status `NOT STARTED`.
- Dowody P01: dla kodu `53204b7e68050d8aa577e171142290a3031f533b` **6/8 wariantów CI PASS**: Ubuntu bundled, Debian i Windows Server ON/OFF. PASS także clang-format i Linux amd64 race. Bindingi Linux/Windows mają osobny PASS dla syntetycznego merge `8215547c8b30176e89d2893901bae953faa6f2d3`. Hosted macOS: [próba 4](https://github.com/dlubom/HDMapping/actions/runs/43886034883/attempts/4) ponowiła tylko brakujące ON/OFF. Oba joby (`133469438125`, `133469438560`) zakończyły się o **17:26:50 UTC** z `runner_id: 0` i `steps: []` (**NOT RUN**; workflow **FAIL infrastruktury**). Adnotacje potwierdzają brak przydziału i ograniczoną pojemność runnerów macOS ARM64; logi zwracają 404 `BlobNotFound`, brak artefaktów i wykonanego checkoutu. Nie ponawiano zielonych kontroli. Lokalnie na tym kodzie: Go z kontrolami negatywnymi, adapter ON (CTest 1/1 i relokacja zachowanego bundla), pełny OFF (CTest 5/5, instalacja, relokacja bundla i odczyt DMG) — PASS we wskazanym zakresie. [Raport P01](results/P01.md#kontynuacja-odbioru--próba-4-macos) podaje wynik próby 4; zachowuje osobno dowody lokalne, wcześniejsze CI macOS ON/DMG dla `4697487` i potwierdzone sumy istniejących pakietów. Kontynuacja od `ecdeaa84cc7cd9e19651a9b2e5abf9696ab8545e` zmienia tylko raport/status; przegląd nie wykazał nowej usterki kodu.
- Braki P00 (bez ponawiania w tej kontynuacji): Windows 11, Claude Desktop, pełny test klienta/GUI Ubuntu, działający Linux TSan, potwierdzone nakładanie i bazowy projekt kroku 3 oraz zapis/odczyt wyniku optymalizacji GUI kroku 2.
- Odroczenie nie zmienia wyników kontroli ani wymagań przenośności i nie upoważnia do usuwania istniejących kontroli CI. Braki danych pozostają otwarte; nie są dowodem zgodności przyszłych zmian przetwarzania.
- Kolejne czaty stosują [zasady przekazania](README.md).

| Karta | Stan | Raport |
| --- | --- | --- |
| P00 | INCOMPLETE | [P00](results/P00.md) |
| P01 | INCOMPLETE | [P01](results/P01.md) |
| P02 | NOT STARTED | — |
| P03 | NOT STARTED | — |
| P04 | NOT STARTED | — |
| P05 | NOT STARTED | — |
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
