import { ConsoleLogger, Logger as NestLogger } from '@nestjs/common'
import { createLogger as createWinstonLogger, format, Logger as WinstonLogger, LoggerOptions, transports } from 'winston'

require('dotenv').config()

export const WinstonLoggerConfig = {
  format: format.combine(format.timestamp(), format.json()),
  transports: [new transports.Console()],
}

export const createLogger = (options: LoggerOptions): WinstonLogger => createWinstonLogger(options)

const defaultLogger = createLogger(WinstonLoggerConfig)
const useWinston = process.env.DISABLE_WINSTON_LOGGER !== 'true'

export class CustomLogger extends ConsoleLogger {
  private readonly logger: WinstonLogger = defaultLogger

  log(message: string, context?: string, data?: any): void {
    if (useWinston) {
      this.logger.info(message, { context, pid: process.pid, data })
    } else {
      const dataFormatted = data ? ` ${JSON.stringify(data)}` : ''
      super.log(message + dataFormatted, context)
    }
  }

  error(message: string, trace?: string, context?: string, data?: any): void {
    if (useWinston) {
      this.logger.error(message, { trace, context, pid: process.pid, data })
    } else {
      const dataFormatted = data ? ` ${JSON.stringify(data)}` : ''
      super.error(message + dataFormatted, trace, context)
    }
  }

  warn(message: string, context?: string, data?: any): void {
    if (useWinston) {
      this.logger.warn(message, { context, pid: process.pid, data })
    } else {
      const dataFormatted = data ? ` ${JSON.stringify(data)}` : ''
      super.warn(message + dataFormatted, context)
    }
  }

  debug(message: string, context?: string, data?: any): void {
    if (useWinston) {
      this.logger.debug(message, { context, pid: process.pid, data })
    } else {
      const dataFormatted = data ? ` ${JSON.stringify(data)}` : ''
      super.debug(message + dataFormatted, context)
    }
  }

  verbose(message: string, context?: string, data?: any): void {
    if (useWinston) {
      this.logger.verbose(message, { context, pid: process.pid, data })
    } else {
      const dataFormatted = data ? ` ${JSON.stringify(data)}` : ''
      super.verbose(message + dataFormatted, context)
    }
  }

  static log(message: string, context?: string, data?: any): void {
    if (useWinston) {
      defaultLogger.info(message, { context, pid: process.pid, data })
    } else {
      NestLogger.log(message, context)
    }
  }

  static error(message: string | any, trace?: string, context?: string, data?: any): void {
    const errorMessage: string = typeof message === 'string' ? message : message?.message
    const meta: any = typeof message === 'object' ? message : {}
    if (useWinston) {
      defaultLogger.error(errorMessage, { ...meta, context, trace, pid: process.pid, data })
    } else {
      NestLogger.error(errorMessage, trace, context)
    }
  }

  static warn(message: string, context?: string, data?: any): void {
    if (useWinston) {
      defaultLogger.warn(message, { context, pid: process.pid, data })
    } else {
      NestLogger.warn(message, context)
    }
  }

  static debug(message: string, context?: string, data?: any): void {
    if (useWinston) {
      defaultLogger.debug(message, { context, pid: process.pid, data })
    } else {
      NestLogger.debug(message, context)
    }
  }

  static verbose(message: string, context?: string, data?: any): void {
    if (useWinston) {
      defaultLogger.verbose(message, { context, pid: process.pid, data })
    } else {
      NestLogger.verbose(message, context)
    }
  }
}