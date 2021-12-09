import _compat_pickle
import importlib
import pickle

from .importer import Importer


class PackageUnpickler(pickle._Unpickler):  # type: ignore[name-defined]
    """Package-aware unpickler.

    This behaves the same as a normal unpickler, except it uses `importer` to
    find any global names that it encounters while unpickling.
    """

    def __init__(self, importer: Importer, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._importer = importer
        self.selective_extern_og_pacakges = {}
        if hasattr(self._importer, "selective_extern_packages"):
            for package in self._importer.selective_extern_packages:  # type: ignore[attr-defined]
                self.selective_extern_og_pacakges[package] = importlib.import_module(
                    package
                )

    def find_class(self, module, name):
        # Subclasses may override this.
        if self.proto < 3 and self.fix_imports:
            if (module, name) in _compat_pickle.NAME_MAPPING:
                module, name = _compat_pickle.NAME_MAPPING[(module, name)]
            elif module in _compat_pickle.IMPORT_MAPPING:
                module = _compat_pickle.IMPORT_MAPPING[module]
        mod = self._importer.import_module(module)

        if hasattr(self._importer, "selective_extern_packages"):
            if self._importer._is_selective_extern_node(  # type: ignore[attr-defined]
                module
            ) and self._importer._is_package_or_module_node(  # type: ignore[attr-defined]
                f"{module}.{name}"
            ):
                mod = self.selective_extern_og_pacakges[module]
        return getattr(mod, name)
