import { useEditor, EditorContent } from '@tiptap/react'
import StarterKit from '@tiptap/starter-kit'
import Image from '@tiptap/extension-image'
import { useEffect, useRef } from 'react'
import { api } from '../../lib/api.js'

export default function RichTextEditor({ value, onChange }) {
  const fileInputRef = useRef(null)

  const editor = useEditor({
    extensions: [StarterKit, Image],
    content: value || '',
    onUpdate: ({ editor }) => onChange(editor.getHTML()),
    editorProps: {
      handlePaste: (view, event) => {
        const file = [...(event.clipboardData?.items ?? [])].find((it) => it.type.startsWith('image/'))?.getAsFile()
        if (!file) return false
        event.preventDefault()
        insertImage(file)
        return true
      },
      handleDrop: (view, event) => {
        const file = [...(event.dataTransfer?.files ?? [])].find((f) => f.type.startsWith('image/'))
        if (!file) return false
        event.preventDefault()
        insertImage(file)
        return true
      },
    },
  })

  useEffect(() => {
    if (editor && value !== editor.getHTML()) editor.commands.setContent(value || '', false)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [value, editor])

  const insertImage = async (file) => {
    try {
      const { url } = await api.uploadImage(file)
      editor.chain().focus().setImage({ src: url }).run()
    } catch {
      window.alert('Không thể tải ảnh lên.')
    }
  }

  const pickFile = () => fileInputRef.current?.click()

  const onFileChange = (e) => {
    const file = e.target.files?.[0]
    e.target.value = ''
    if (file) insertImage(file)
  }

  if (!editor) return null

  return (
    <div className="rte">
      <div className="rte__toolbar">
        <button type="button" onClick={() => editor.chain().focus().toggleBold().run()} className={editor.isActive('bold') ? 'is-active' : ''}>
          B
        </button>
        <button type="button" onClick={() => editor.chain().focus().toggleItalic().run()} className={editor.isActive('italic') ? 'is-active' : ''}>
          I
        </button>
        <button type="button" onClick={() => editor.chain().focus().toggleHeading({ level: 2 }).run()} className={editor.isActive('heading', { level: 2 }) ? 'is-active' : ''}>
          H2
        </button>
        <button type="button" onClick={() => editor.chain().focus().toggleBulletList().run()} className={editor.isActive('bulletList') ? 'is-active' : ''}>
          • List
        </button>
        <button type="button" onClick={pickFile}>
          Chèn ảnh
        </button>
        <input ref={fileInputRef} type="file" accept="image/*" hidden onChange={onFileChange} />
      </div>
      <EditorContent editor={editor} className="rte__content" />
    </div>
  )
}
