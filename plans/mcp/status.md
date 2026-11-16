# Status MCP

Plan zaakceptowany. P00 wykonano częściowo; wynik **INCOMPLETE**. Właściciel 2026-11-16 polecił odłożyć kwalifikację Windows, Linux i klienta Claude oraz przejść dalej: **P01 rozpoczęto mimo niepełnego P00; implementacja, dostępne kontrole platformowe i publikacja wiki są dostarczane jako INCOMPLETE**. Szczegóły, dowody i zakres tej decyzji: [raport P00](results/P00.md#decyzja-właściciela-po-raporcie).

Uzgodniony [standard jakości Go/C++](quality.md) jest częścią planu: P00 kwalifikuje narzędzia, P01 wdraża kontrole Go, P03 obejmuje nimi nowe moduły C++. Wdrożone kontrole Go i ich wykonany zakres opisuje raport P01; samo dodanie konfiguracji nie zalicza pełnej macierzy.

**Decyzja właściciela 2026-11-16:** testy rzeczywistego klienta Claude odkładamy do końcowego odbioru P17. Właściciel nie ma obecnie Claude Code; nie instalujemy ani nie kwalifikujemy teraz klienta Claude. W uzgodnionym planie hostem jest Claude Desktop — odroczenie nie zamienia go automatycznie na Claude Code. Brak klienta nie blokuje prac nad osobno zleconymi kartami; jego testy pozostają `NOT RUN` i muszą wrócić w P17 przed pełnym odbiorem oraz domyślnym włączeniem MCP. Pozostałe braki P00/P01 i ograniczenie jednej karty na czat pozostają jawne.

- Repozytorium do zapisu: `dlubom/HDMapping`.
- Branch planu: `mcp/implementation-plan`.
- Bazowy commit rozpoznania: `3d3a8054986f95fcf580ef0f780889043bfdd475`.
- Baza P01: `8d8ec2a15099e13dba5237b16a5e70a2b324b2fd`; badany kod P01: `46974875db17aabe17b1a292eb314df1abdfd65f`; wspólna gałąź utworzona od `bb8b988b9be6dd26fc2541580bce2edfb962144e`.
- Branch kwalifikacji: `mcp/p00`.
- Decyzja właściciela 2026-11-16: wspólna gałąź `mcp/development` i jeden [draft PR #1](https://github.com/dlubom/HDMapping/pull/1) do `main` wyłącznie w forku. Gałąź utworzona z `bb8b988b9be6dd26fc2541580bce2edfb962144e`; `mcp/p01` zachowany. Ta zmiana sposobu dostarczenia nie upoważnia do merge, release ani rozpoczęcia kolejnych kart.
- Następna praca: **kontynuacja P01 na `mcp/development`** od zdalnego SHA z przekazania, w istniejącym draft PR. [Raport P01](results/P01.md): PASS CI ON/OFF Windows Server, Ubuntu bundled i Debian (pełny build, testy, install/relokacja), Linux amd64 race, macOS ON/bundle/DMG, wiki i formatowanie. Otwarte: niewykonane CI macOS OFF po trzech próbach bez przydziału runnera ARM64 (workflow FAIL infrastruktury) oraz rzeczywisty Windows 11. Claude zostaje na P17. P02 nie rozpoczęto.
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
