# Introduction
The files in here are used internally within u-blox to automate testing of `ubxlib`.  They are not supported externally.  However, if you find them useful please help yourselves.

# Reading The Jenkins Output
Since much of the test execution is inside these Python scripts the Jenkins level Groovy script `Jenkinsfile` doesn't do a great deal.  To see how testing has gone look in the Jenkins artifacts for files named `summary.log`.  There should be one at the top level and another inside the folder for each instance.  Check the top level `summary.log` to see how the run went.  The `summary.log` file inside each instance folder contains the portion of the summary for that instance but adds no aditional information so, if there is a problem, check the `debug.log` file inside the instance folder for more detailed information.

The best approach to looking for failures is to search for " \*\*\* ", i.e. a space, three asterisks, then a space, in these files.  Search first in the top-level summary log file to find any failures or warnings and determine which instance caused them.  When you have determined the instance, go to the directory of that instance, open the debug log file there and search for the same string again to find the failure and warning markers within the detailed trace.

# Running Scripts Locally
**NOTE:** if you were running scripts locally and then, after updating your code from the repo, you find that you get strange errors, try backing up and then deleting the contents of the `.ubx_automation` directory off your user home directory.  The error might be because a value in the settings has been updated and your local settings file is out of date, in which case this will fix it.  If you had any local overrides to the settings you can then merge them back into the new settings file from your back-up copy.

You may need to run the automated tests locally, e.g. when sifting through Lint issues or checking for Doxygen issues, or simply running tests on a locally-connected board in the same way as they would run on the automated test system.  To do this, assuming you have the required tools installed (the scripts will often tell you if a tool is not found and give a hint as to where to find it), the simplest way to do this is to `CD` to this directory and run, for instance:

```
python u_run.py 0 -w c:\temp -u c:\projects\ubxlib_priv -d debug.log
```

...where `0` is the instance you want to run (in this case Lint), `c:\temp` is a temporary working directory (replace as appropriate), `c:\projects\ubxlib_priv` the location of the root of this repo (replace as appropriate) and `debug.log` a place to write the detailed trace information.

If you are trying to run locally a test which talks to real hardware you will also need to edit the file `settings.json` file, which is stored in the `.ubx_automation` directory off the current user's home directory. Override the debugger serial number and/or COM port the scripts would use for that board on the automated test system. Assuming you only have one board connected to your PC, locate the entry for the instance you plan to run in the top of that file; there set `serial_port` to the port the board appears as on your local machine and set `debugger` (if present) to `None`.  For instance, to run instance 16 locally you might open that file and change:

```
  "CONNECTION_INSTANCE_16": {
    "serial_port": "COM7",
    "debugger": "683920969"
  }
```

...to:

```
  "CONNECTION_INSTANCE_16": {
    "serial_port": "COM3",
    "debugger": None
  }
```

By setting `debugger` to `None`, the script will simply pick the one and only connected board. Would there be multiple boards connected to the PC, one must specify corresponding serial number for debugger. 

# Script Usage
The main intended entry point into automation is the `u_pull_request.py` Python script.  You can run it with parameter `-h` to obtain usage information but basically the form is:

`u_pull_request.py "some text" <a list of file paths, unquoted, separated with spaces>`

The text is intended to be that submitted with the pull request.  When `u_pull_request.py` is called from Jenkins, using `Jenkinsfile`, the pull request text is grabbed from Github by the `Jenkinsfile` script.  This text is parsed for a line that starts with `test: foo bar`, where `foo` is either `*` or a series of digits separated by spaces or full stops, e.g. `0` `0 1` or `1.1` or `4.0.2 6.1.3` and `bar` is some more optional text, e.g. `test: 1 example`.  `foo` indicates the instance IDs to run from `DATABASE.md`, separated by spaces, and `bar` is a filter string, indicating that only examples/tests that begin with that string should be run.  For instance `test: 1 2 3 example` would run all things that begin with `example` on instance IDs `1`, `2` and `3`, or `test: * port` would run *all* things that begin with `port` (i.e. the porting tests) on *all* instances, or `test: *` would run everything on all instance IDs, etc.

So, when submitting a pull request if a such a line of text is included with it then the u-blox Jenkins configuration will parse the text, find the `test:` line and conduct those tests on the pull request, returning the results to Github.

Note: on the ESP-IDF platform, which comes with its own unit test implementation, the filter string must be the full name of a category, e.g. `port` or `example`, partial matches are not supported.

If a line starting with `test:` is *not* included (the usual case) then the file list must be provided.  Again, when `u_pull_request.py` is called from Jenkins, `Jenkinsfile`, will grab the list of changed files from Github.  `u_pull_request.py` then calls the `u_select.py` script to determine what tests should be run on which instance IDs to verify that the pull request is good.  See the comments in `u_select.py` to determine how it does this.  It is worth noting that the list of tests/instances selected by `u_select.py` will always be the largest one: e.g. if a `.h` file in the `port` API of `ubxlib` has been changed then all the `port` tests on all instance IDs will be selected.  To narrow the tests/instances that are run, use the `test:` line to specify it yourself.  Note that though you can edit the pull request submission text unfortunately this does not retrigger testing, only pushing another commit to the pull request will do that.

For each instance ID the `u_run.py` script, the thing that ultimately does the work, will return a value which is zero for success or otherwise the number of failures that occurred during the run.  Search the output from the script for the word `EXITING` to find its return value.

# Script Descriptions
`Jenkinsfile`: tells Jenkins what to do, written in Groovy stages.  Key point to note is that the archived files will be `summary.log` for a quick overview, `test_report.xml` for an XML formatted report on any tests that are executed on the target HW and `debug.log` for the full detailed debug on each build/download/run.

`u_connection.py`: contains a record of how each item of HW is connected into the test system (COM port, debugger serial number, etc. collected from the settings) and functions to access this information.  All of the connections are retrieved-from/stored-in the settings file (see below).

`u_data.py`: functions to parse `DATABASE.md`.

`u_monitor.py`: monitors the output from an instance that is executing tests, checking the output for passes/failures and optionally writing debug logs and an XML report to file.

`u_pull_request.py`: see above.  `Jenkinsfile`, for instance, would run the following on a commit to a branch of `ubxlib`:

```
python u_pull_request.py "test: *" -w z:\_jenkins_work -u z:\ -s summary.log -d debug.log -t report.xml
```

...where `z:` has been `subst`ed to the root of the `ubxlib` directory (and the directory `_jenkins_work` created beforehand).

`u_report.py`: write reports on progress.

`u_run.py`: handles the build/download/run of tests on a given instance by calling one of the `u_run_xxx.py` scripts below; this script is called multiple times in parallel by `u_pull_request.py`, each running in a Python process of its own.  If, for instance, you wanted to run all the tests on instance 0 as `Jenkinsfile` would, you might run:

```
python u_run.py 0 -w z:\_jenkins_work -u z:\ -s summary.log -d debug.log -t report.xml
```

...with `z:` `subst`ed to the root of the `ubxlib` directory (and the directory `_jenkins_work` created beforehand).  This is sometimes useful if `u_pull_request.py` reports an exception but can't tell you where it is because it has no way of tracing back into the multiple `u_run.py` processes it would have launched.

`u_settings.py`: stores and retrieves the paths etc. used by the various scripts.  The settings file is `settings.json` in a directory named `.ubx_automation` off the current user's home directory.  If no settings file exists a default one is first written.  If new settings are added when an existing settings file exists then they are merged into it and stored to preserve backwards-compatibility. If you **modify** an existing setting in `u_settings.py` you must delete the `.ubx_automation` directory for the new default settings to be written and read back into your script. **IMPORTANT**: if you do this while testing your branch you **must**, temporarily, change the name of the settings file used by the `u_settings.py` script, e.g. to something like `settings_my_change_name.json`.  This is because other branches (e.g. `master`) being run on the same test machine will be using the original settings and your change will mess them up.  Once your PR is merged you must then submit a subsequent PR to change the name back again to `settings.json`.  **ALSO IMPORTANT**: if you commit a change which **modifies** an existing setting in `u_settings.py` you must make sure that everyone using these scripts deletes their `.ubx_automation` directory (or updates their `.ubx_automation/settings.json` file with the changed value(s)) for them to adopt the new setting; their local scripts may fail until this is done, because they will have the wrong value for whatever setting you changed.

`u_run_astyle.py`: run an advisory-only (i.e. no error will be thrown ever) AStyle check; called by `u_run.py`.  NOTE: because of the way AStyle directory selection works, if you add a new directory off the `ubxlib` root directory (i.e. you add something like `ubxlib\blah`) YOU MUST ALSO ADD it to the `ASTYLE_DIRS` variable of this script.  To AStyle your files before submission, install `AStyle` version 3.1 and, from the `ubxlib` root directory, run:

```
astyle --options=astyle.cfg --suffix=none --verbose --errors-to-stdout --recursive *.c,*.h,*.cpp,*.hpp
```

`u_run_doxygen.py`: run Doxygen to check that there are no documentation errors; called by `u_run.py`.

`u_run_esp_idf.py`: build/download/run tests for the Espressif ESP-IDF platform, e.g. for the ESP32 MCU; called by `u_run.py`.

`u_run_lint.py`: run a Lint check; called by `u_run.py`.  NOTE: if you add a NEW DIRECTORY containing a PLATFORM INDEPENDENT `.c` or `.cpp` file anywhere in the `ubxlib` tree YOU MUST ALSO ADD it to the `LINT_DIRS` variable of this script.  All the Lint error/warning/information messages available for GCC are included with the exception of those suppressed by the `ubxlib.lnt` configuration file kept in the `port\platform\lint` directory.  To run Lint yourself you must install a GCC compiler and `flexelint`: read `u_run_lint.py` to determine how to go about using the tools.

`u_run_nrf5sdk.py`: build/download/run tests on the Nordic nRF5 platform, e.g. for the NRF52 MCU; called by `u_run.py`.

`u_run_zephyr.py`: build/download/run tests on the Zephyr platform, e.g. for the NRF53 MCU; called by `u_run.py`.

`u_run_pylint.py`: run Pylint on all of these Python automation scripts; called by `u_run.py`.

`u_run_stm32cube.py`: build/download/run tests on the ST Microelectronic's STM32Cube platform, e.g. for an STM32F4 MCU; called by `u_run.py`.

`u_select.py`: see above.

`u_utils.py`: utility functions used by all of the above.

# Maintenance
- If you add a new API make sure that it is listed in the `APIs available` column of at least one row in `DATABASE.md`, otherwise `u_select.py` will **not**  select it for testing on a Pull Request.
- If you add a new board to the test machine or change the COM port or debugger serial number that an existing board uses on the test machine, update `u_connection.py` to match.
- If you add a new platform or test suite, add it to `DATABASE.md` and make sure that the result is parsed correctly by `u_data.py` (e.g. by running `u_pull_request.py` from the command-line and checking that everything is correct).
- If you add a new item in the range 0 to 9 (i.e. a checker with no platform), update `u_run.py` to include it.
- If you add a new directory OFF THE ROOT of `ubxlib`, i.e. something like `ubxlib\blah`, add it to the `ASTYLE_DIRS` variable of the `u_run_astyle.py` script.
- If you add a new directory that contains PLATFORM INDEPENDENT `.c` or `.cpp` files anywhere in the `ubxlib` tree, add it to the `LINT_DIRS` variable of the `u_run_lint.py` script.