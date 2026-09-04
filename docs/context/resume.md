# Cedric de Wijs — Résumé (project context)

This is the developer profile behind PickupGameOrganizer. It is checked in as
context so that anyone (human or agent) working on this repo knows which
technologies, tooling, and engineering practices the author is fluent in, and
can pick approaches that match. Text extracted from
[`resume.pdf`](resume.pdf) — the PDF is the authoritative copy.

**Senior Embedded Linux Software Developer** — Haarlem, Netherlands
cedric.dewijs@eclipso.eu · [github.com/cdwijs](http://github.com/cdwijs) · [LinkedIn](http://linkedin.com/in/cedric-de-wijs-610b51b9/)

## Summary

Senior Embedded Linux & C++ engineer with 20+ years of experience developing
industrial and medical embedded systems. Specialized in embedded Linux, C/C++,
real-time control systems, CI/CD automation, and hardware/software integration
for high-reliability environments. Experienced in leading architecture
decisions, mentoring engineers, and delivering robust systems for industrial
automation and medical research applications.

## Skills

**Core:** C++, Qt, Qwt development · embedded Linux development · GitLab CI/CD
build servers and pipelines · Git, GitLab and GitHub version control · vertical
farming equipment · CANopen debugging · distributed industrial control systems
and fieldbus communication architectures · Yocto

**Additional:** embedded C development · control systems engineering · EMC
testing · PCBA design using Altium and KiCAD · code reviews · quality assurance
systems · kernel development · device drivers · cross-compilation · networking ·
GDB · profiling/debugging tools · memory optimization · ARM architectures ·
embedded filesystems · secure update mechanisms

**Soft skills:** technical leadership and mentoring · ability to explain complex
concepts in simple terms · ability to work independently and take a leading
role · ability to divide up complex systems into individual components

**Languages:** Dutch (native speaker), English (fluent), German (basic),
French (basic)

## Education

**Hogeschool Inholland Haarlem** — Bachelor of Electrical Engineering
*Jul 1999 – Jul 2003*

## Work experience

### Airlux Technologies — Senior Embedded Hardware Engineer
*Mar 2022 – present*

- Designed and delivered embedded control systems for climate-controlled
  environments in vertical farming, achieving ±0.3 °C temperature and ±3 %
  humidity stability.
- Ported embedded firmware to Qt/C++ based simulation environments for
  automated testing and fast debug cycles.
- Ported embedded PIC18 code to a 64-bit Linux environment, enabling automated
  unit testing and end-to-end testing on a build server.
- Established GitHub based CI/CD pipelines for automatic unit testing and
  firmware releases, with versioning based on git tags and branch names.
- Performed code reviews, safeguarding software quality.
- Mentored colleagues on software architecture, unit testing frameworks, git
  workflows, and CI/CD pipelines.
- Analyzed and resolved complex software defects.
- Collaborated with international software and hardware teams.
- Implemented PCBA improvements based on EMC testing, significantly improving
  signal integrity and consequently system accuracy.

*Environment:* C, Unity, PIC18, C++, Qt, GitHub CI/CD, Linux, Git, Altium,
KiCAD, FreeCAD, RS485, I2C, Modbus, Valgrind
*Quality assurance processes:* test-driven development, static analysis, code
coverage, Agile/Scrum, release engineering, branching strategies, documentation
standards, architecture review, code review

### Leiden University Medical Center (LUMC) — Senior Instrument Maker / Embedded Software Engineer
*Mar 2013 – Mar 2022*

- Created a C++/Qt program for circadian rhythm research, controlling lights via
  a DALI bus so researchers can create custom day/night patterns.
- Developed embedded Linux systems for remote research setups.
- Developed precision electronic systems for medical and research applications,
  for instance a low-noise control system achieving ±0.1 °C stability for
  sensitive biological experiments.
- Created embedded software according to IEC 62304 medical software standards
  and MDR compliance requirements.
- Modified medical hardware while maintaining MDR compliance requirements.
- Introduced GitLab CI/CD pipelines and emulation environments for ATmega
  microcontrollers.
- Performed root-cause analysis and resolved complex hardware/software
  integration defects.
- Collaborated closely with researchers, clinicians, and the mechanical team to
  translate requirements into robust solutions.
- Provided technical guidance and mentoring to colleagues and trainees.

*Environment:* C, Unity, ATmega, emulation, C++, Qt, GitLab CI/CD, Linux, Git,
Altium, KiCAD, FreeCAD, DALI, I2C, RS232, LabVIEW, IEC 62304

### Quant Consultancy Electronics — Hardware & Software Engineer
*Mar 2008 – Mar 2013*

- Designed embedded software in C and assembly for reliable real-time systems.
- Ported legacy code to more modern platforms (PIC16/18/24), reducing
  production costs.
- Conducted EMC testing and redesigns to improve compliance and reduce
  certification risk.
- Designed and delivered PCBAs for high volume production.

*Environment:* C, assembly, PIC16/PIC18/PIC24, Linux, Git, Altium, KiCAD, I2C,
RS232

### Beckhoff New Automation Technology — Support Engineer
*Mar 2004 – Mar 2008*

- Supported customers using Beckhoff control systems based on field buses such
  as CANopen, Profibus, and EtherCAT.
- Resolved problems in complicated configurations and large PLC programs by
  reproducing them in minimal configurations, reporting them to the software
  team, and delivering the solutions back to the customer.
- Established a tracking system for support requests and solutions, virtually
  eliminating response times for common questions.
- Created documentation change tracking processes, reducing the time before new
  features of the supported hardware and software were discovered.
- Reviewed technical proposals for incompatible system combinations, reducing
  return and exchange costs.

*Environment:* TwinCAT, IEC 61131-3, EtherCAT, CANopen, Profibus, real-time
Ethernet, Lightbus

## Selected embedded Linux project

**High-performance multi-channel DVR system**

- Designed and developed a Linux-based digital video recorder capable of
  recording 22 DVB-T channels simultaneously.
- Patched Linux kernel drivers so the system suspends and resumes reliably when
  a USB receiver that is in use is disconnected.
- Ported the system from Arch Linux on x86 hardware to Debian on an Allwinner
  A20 ARM SoC, reducing power consumption by over 90 %.
- Documented the development process on the Arch Linux and Debian wiki
  platforms.

*Environment:* VDR, Debian, Arch Linux, Allwinner A20, Digitenne

## Volunteer experience

- Volunteering at open source conferences including FOSDEM (Belgium) and
  FrOSCon (Germany).
- Volunteering at Linux repair cafés, migrating systems from Windows to Linux
  while keeping the ability to run needed Windows programs.
- Volunteering at LaptopRevive.nl, installing Linux on donated laptops for
  students who cannot afford them.
- Founder of the soccer club "Terrible football Haarlem".
