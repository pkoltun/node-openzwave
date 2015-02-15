{
  "targets": [
    {
      "target_name": "openzwave",
      "sources": [
        "src/openzwave.cc"
      ],
      "include_dirs": [
        "deps/open-zwave/cpp/src"
        "deps/open-zwave/cpp/hidapi/hidapi",
        "deps/open-zwave/cpp/src",
		"deps/open-zwave/cpp/src/aes",
        "deps/open-zwave/cpp/src/command_classes",
        "deps/open-zwave/cpp/src/platform",
        "deps/open-zwave/cpp/src/value_classes",
        "deps/open-zwave/cpp/tinyxml"
      ],
	   'conditions': [
        ['OS=="linux"', {
          "include_dirs": [ "deps/open-zwave/cpp/src/platform/unix" ],
		}],
        ['OS=="win"', {
          "include_dirs": [ "deps/open-zwave/cpp/src/platform/windows" ],
		  'msvs_settings': {
            'VCLinkerTool': {
              'AdditionalDependencies': [
                'setupapi.lib',
              ]
            }
          }
		}]
		],
      "dependencies": [
        "deps/open-zwave/libopenzwave.gyp:libopenzwave"
      ]
    }
  ]
}
