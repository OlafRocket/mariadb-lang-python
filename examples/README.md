# lang_python examples

## Examples

Each file contains one or more examples of a realistic use of lang_python.

The examples define named functions in a shared `example_` namespace:
each file registers its Python code once with `py_replace_function()` and
then calls it with `py_call()`. Named functions are compiled once, stored
in the `mysql.lang_python_func` catalog, and survive server restarts, so
the functions of an example file stay available after the file runs, like
any other function. Catalog names are flat identifiers (`[A-Za-z0-9_]`),
so the `example_` prefix acts as the namespace. `py_replace_function()`
makes the files idempotent: running a file again redefines its functions.

The files:

- `slack_send` shows a trigger that sends a Slack message when a big payment
  is received (`example_slack_send`). Configuration is read through a SQL
  helper function.
- `pagerduty_send` is similar, but a PagerDuty incident is triggered instead
  (`example_pagerduty_incident`), which alerts a support team. Unlike
  `slack_send`, the Python body reads its configuration itself, with the
  built-in `inhost` module (in-process SQL access).
- `ping` shows how to measure the latency of another MariaDB server (or any
  other host). `example_ping` sends a ping; `example_ping_stats` runs a
  series of pings and calculates statistics (min, max, average,
  standard deviation). Requires the `ping3` module.
- `misc` shows file size checks (`example_undo_size`,
  `example_schema_size`): basic checks on the server's health implemented
  with lang_python. `example_type_of` returns the Python type of a SQL
  value.
- `get_python_path` returns the interpreter path (`example_python_path`), so
  an external tool knows where to `pip install` modules.

## Running the examples

`run.sh` pipes a file through the `mariadb` client with `-vvv --comments`,
so each statement, Python source included, is echoed before its result:

```sh
./run.sh                        # all side-effect-free examples
./run.sh misc -h 127.0.0.1 -u root
./run.sh slack_send             # optional, must be named explicitly
```

Extra arguments are passed to the client. `slack_send` and
`pagerduty_send` are optional: they post to real endpoints, so the
no-argument form skips them and they run only when named explicitly.
Set the webhook URL / routing key in their `config` tables first.

List the installed functions and clean up:

```sql
SELECT name, return_type, param_names FROM mysql.lang_python_func
    WHERE name LIKE 'example\_%';
SELECT py_drop_function('example_ping');            -- and so on
DROP SCHEMA IF EXISTS xmp;                          -- slack/pagerduty demo objects
```

## To-Do

- Run a specified query and return the results as YAML.
- Return a customised text from a Jinja2 template.
- Send events to RabbitMQ.
- A shared in-interpreter cache for aggregated data (e.g. today's total
  money in), using the `sys.modules` shared-state pattern from USAGE.md.
  Depends on the concurrency model that the free-threaded CPython work
  (ROADMAP Phase 4) settles.
