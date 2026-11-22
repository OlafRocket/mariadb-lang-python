package My::Suite::Lang_python;

@ISA = qw(My::Suite);

return "No LANG_PYTHON plugin" unless $ENV{LANG_PYTHON_SO};

sub is_default { 1 }

bless { };
