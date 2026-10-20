import { EditorContent, useEditor, type Editor } from '@tiptap/react'
import StarterKit from '@tiptap/starter-kit'
import Image from '@tiptap/extension-image'
import { useEffect, useRef } from 'react'
import { useI18n, type StringKey } from '../i18n'
import { CmsError, cmsApi } from './cmsApi'

type Props = { value: string; onChange: (html: string) => void; onError: (message: string) => void }

type Tool = { key: StringKey; label: string; run: (e: Editor) => void; active?: (e: Editor) => boolean; disabled?: (e: Editor) => boolean }

function RichTextEditor({ value, onChange, onError }: Props) {
  const { t } = useI18n()
  const fileInput = useRef<HTMLInputElement>(null)
  const editorRef = useRef<Editor | null>(null)

  const insertImage = async (file: File) => {
    try {
      const { url } = await cmsApi.uploadImage(file)
      editorRef.current?.chain().focus().setImage({ src: url }).run()
    } catch (err) {
      onError(t('cms.editor.uploadFailed', { error: err instanceof CmsError ? err.message : String(err) }))
    }
  }

  const editor = useEditor({
    extensions: [StarterKit.configure({ link: { openOnClick: false } }), Image],
    content: value || '',
    // Toolbar active/disabled states read editor state during render.
    shouldRerenderOnTransaction: true,
    onUpdate: ({ editor: e }) => onChange(e.getHTML()),
    editorProps: {
      handlePaste: (_view, event) => {
        const file = [...(event.clipboardData?.items ?? [])].find((it) => it.type.startsWith('image/'))?.getAsFile()
        if (!file) return false
        event.preventDefault()
        void insertImage(file)
        return true
      },
      handleDrop: (_view, event) => {
        const file = [...(event.dataTransfer?.files ?? [])].find((f) => f.type.startsWith('image/'))
        if (!file) return false
        event.preventDefault()
        void insertImage(file)
        return true
      },
    },
  })

  useEffect(() => {
    editorRef.current = editor
  }, [editor])

  // External value changes (post loaded after mount) -- skip echoes of our own onUpdate.
  // isDestroyed: StrictMode's remount re-runs this against the editor instance
  // useEditor already destroyed, whose schema can no longer serialize.
  useEffect(() => {
    if (!editor || editor.isDestroyed) return
    if (value !== editor.getHTML()) editor.commands.setContent(value || '', { emitUpdate: false })
  }, [value, editor])

  if (!editor) return null

  const setLink = (e: Editor) => {
    const url = window.prompt(t('rte.linkPrompt'), e.getAttributes('link').href ?? '')
    if (url === null) return
    if (url.trim() === '') e.chain().focus().extendMarkRange('link').unsetLink().run()
    else e.chain().focus().extendMarkRange('link').setLink({ href: url.trim() }).run()
  }

  const tools: Tool[] = [
    { key: 'rte.bold', label: 'B', run: (e) => e.chain().focus().toggleBold().run(), active: (e) => e.isActive('bold') },
    { key: 'rte.italic', label: 'I', run: (e) => e.chain().focus().toggleItalic().run(), active: (e) => e.isActive('italic') },
    { key: 'rte.h2', label: 'H2', run: (e) => e.chain().focus().toggleHeading({ level: 2 }).run(), active: (e) => e.isActive('heading', { level: 2 }) },
    { key: 'rte.h3', label: 'H3', run: (e) => e.chain().focus().toggleHeading({ level: 3 }).run(), active: (e) => e.isActive('heading', { level: 3 }) },
    { key: 'rte.bullet', label: '•', run: (e) => e.chain().focus().toggleBulletList().run(), active: (e) => e.isActive('bulletList') },
    { key: 'rte.ordered', label: '1.', run: (e) => e.chain().focus().toggleOrderedList().run(), active: (e) => e.isActive('orderedList') },
    { key: 'rte.quote', label: '❝', run: (e) => e.chain().focus().toggleBlockquote().run(), active: (e) => e.isActive('blockquote') },
    { key: 'rte.link', label: '🔗', run: setLink, active: (e) => e.isActive('link') },
    { key: 'rte.image', label: '🖼', run: () => fileInput.current?.click() },
    { key: 'rte.undo', label: '↶', run: (e) => e.chain().focus().undo().run(), disabled: (e) => !e.can().undo() },
    { key: 'rte.redo', label: '↷', run: (e) => e.chain().focus().redo().run(), disabled: (e) => !e.can().redo() },
  ]

  return (
    <div className="rte">
      <div className="rte-toolbar" role="toolbar">
        {tools.map((tool) => (
          <button
            key={tool.key}
            type="button"
            title={t(tool.key)}
            aria-label={t(tool.key)}
            aria-pressed={tool.active ? tool.active(editor) : undefined}
            className={tool.active?.(editor) ? 'rte-btn rte-btn--active' : 'rte-btn'}
            disabled={tool.disabled?.(editor)}
            // Keep focus/selection in the editor: otherwise the click blurs it and
            // the first keystrokes after e.g. Bold land before focus returns.
            onMouseDown={(e) => e.preventDefault()}
            onClick={() => tool.run(editor)}
          >
            {tool.label}
          </button>
        ))}
        <input
          ref={fileInput}
          type="file"
          accept="image/*"
          hidden
          onChange={(e) => {
            const file = e.target.files?.[0]
            e.target.value = ''
            if (file) void insertImage(file)
          }}
        />
      </div>
      <EditorContent editor={editor} className="rte-content" />
    </div>
  )
}

export default RichTextEditor
