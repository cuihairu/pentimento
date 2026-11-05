import { defineConfig } from 'vitepress'

export default defineConfig({
  base: '/pentimento/',
  title: 'pentimento',
  head: [['link', { rel: 'icon', type: 'image/svg+xml', href: '/pentimento/favicon.svg' }]],
  description:
    'A local desktop asset manager for game art assets, centered on version-history browsing and restore.',
  srcExclude: [
    'zh/design/**',
    'zh/review/**',
    'zh/review-log.md',
    'zh/desktop-prototype-spec.md',
    'zh/integrations.md',
    'zh/performance.md',
    'zh/requirements.md'
  ],
  locales: {
    root: {
      label: 'English',
      lang: 'en-US',
      themeConfig: {
        nav: [
          { text: 'Home', link: '/' },
          { text: 'Quickstart', link: '/quickstart' },
          { text: 'Configuration', link: '/configuration' },
          { text: 'Architecture', link: '/architecture' }
        ],
        sidebar: [
          {
            text: 'Docs',
            items: [
              { text: 'Quickstart', link: '/quickstart' },
              { text: 'Configuration', link: '/configuration' },
              { text: 'Architecture', link: '/architecture' }
            ]
          }
        ]
      }
    },
    zh: {
      label: '简体中文',
      lang: 'zh-CN',
      link: '/zh/',
      themeConfig: {
        nav: [
          { text: '首页', link: '/zh/' },
          { text: '快速开始', link: '/zh/quickstart' },
          { text: '配置', link: '/zh/configuration' },
          { text: '架构', link: '/zh/architecture' }
        ],
        sidebar: [
          {
            text: '文档',
            items: [
              { text: '快速开始', link: '/zh/quickstart' },
              { text: '配置', link: '/zh/configuration' },
              { text: '架构', link: '/zh/architecture' }
            ]
          }
        ]
      }
    }
  },
  themeConfig: {
    logo: '/logo.svg',
    socialLinks: [{ icon: 'github', link: 'https://github.com/cuihairu/pentimento' }]
  }
})
