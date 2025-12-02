; Inject JavaScript into script elements
((script_element
  (script_content) @injection.content)
  (#set! injection.language "javascript"))

; Inject CSS into style elements
((style_element
  (style_content) @injection.content)
  (#set! injection.language "css"))
